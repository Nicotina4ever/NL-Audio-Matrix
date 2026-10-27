#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
    struct Options
    {
        juce::String input = "NL Mic";
        juce::String output = "NLMatrix";
        double seconds = 3.0;
        double toneHz = 440.0;
        juce::String wavPath;
        juce::String outPath;
        bool list = false;
    };

    Options parse (int argc, char** argv)
    {
        Options o;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a (argv[i]);
            if (a == "--list") o.list = true;
            else if (a == "--input" && i + 1 < argc) o.input = argv[++i];
            else if (a == "--output" && i + 1 < argc) o.output = argv[++i];
            else if (a == "--seconds" && i + 1 < argc) o.seconds = std::atof (argv[++i]);
            else if (a == "--tone-hz" && i + 1 < argc) o.toneHz = std::atof (argv[++i]);
            else if (a == "--play-wav" && i + 1 < argc) o.wavPath = argv[++i];
            else if (a == "--out" && i + 1 < argc) o.outPath = argv[++i];
        }
        return o;
    }

    juce::String findDeviceName (const juce::StringArray& names, const juce::String& wanted)
    {
        for (const auto& n : names)
            if (n.containsIgnoreCase (wanted))
                return n;
        return {};
    }

    struct ToneCallback final : public juce::AudioIODeviceCallback
    {
        explicit ToneCallback (double hz) : frequency (hz) {}

        void audioDeviceIOCallbackWithContext (const float* const*, int,
                                               float* const* output, int numOutputs,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            for (int ch = 0; ch < numOutputs; ++ch)
            {
                if (output[ch] == nullptr) continue;
                for (int i = 0; i < numSamples; ++i)
                {
                    output[ch][i] = (float) (0.35 * std::sin (phase));
                    phase += 2.0 * 3.14159265358979323846 * frequency / sampleRate;
                    if (phase > 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;
                }
            }
        }
        void audioDeviceAboutToStart (juce::AudioIODevice* d) override
        {
            sampleRate = d != nullptr ? d->getCurrentSampleRate() : 48000.0;
            if (sampleRate <= 0.0) sampleRate = 48000.0;
        }
        void audioDeviceStopped() override {}

        double frequency = 440.0;
        double phase = 0.0;
        double sampleRate = 48000.0;
    };

    struct WavPlaybackCallback final : public juce::AudioIODeviceCallback
    {
        explicit WavPlaybackCallback (const juce::String& path)
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            reader.reset (formats.createReaderFor (juce::File (path)));
            if (reader != nullptr)
            {
                buffer.setSize (juce::jmax (1, (int) reader->numChannels),
                                (int) reader->lengthInSamples);
                reader->read (&buffer, 0, (int) reader->lengthInSamples, 0, true, true);
                sampleRate = reader->sampleRate;
            }
        }
        void audioDeviceAboutToStart (juce::AudioIODevice*) override
        {
            startMs.store (juce::Time::getMillisecondCounterHiRes(), std::memory_order_release);
        }
        void audioDeviceIOCallbackWithContext (const float* const*, int,
                                               float* const* output, int numOutputs,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            for (int ch = 0; ch < numOutputs; ++ch)
                if (output[ch] != nullptr)
                    juce::FloatVectorOperations::clear (output[ch], numSamples);
            if (reader == nullptr || buffer.getNumSamples() == 0)
                return;
            for (int i = 0; i < numSamples && position < buffer.getNumSamples(); ++i, ++position)
                for (int ch = 0; ch < numOutputs; ++ch)
                    if (output[ch] != nullptr)
                        output[ch][i] = buffer.getSample (juce::jmin (ch, buffer.getNumChannels() - 1), position);
        }
        void audioDeviceStopped() override {}
        std::unique_ptr<juce::AudioFormatReader> reader;
        juce::AudioBuffer<float> buffer;
        int position = 0;
        double sampleRate = 48000.0;
        std::atomic<double> startMs { -1.0 };
    };
    struct CaptureCallback final : public juce::AudioIODeviceCallback
    {
        void audioDeviceIOCallbackWithContext (const float* const* input, int numInputs,
                                               float* const*, int, int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            if (numInputs <= 0 || input[0] == nullptr)
                return;
            if (! firstCallbackSeen.exchange (true))
            {
                // Timestamp of the first delivered capture packet: the packet
                // covers [t - numSamples/rate, t] so it lets the latency
                // analyser map WAV sample 0 to a wall-clock time much more
                // accurately than audioDeviceAboutToStart() alone.
                firstCallbackMs.store (juce::Time::getMillisecondCounterHiRes(),
                                       std::memory_order_release);
                firstCallbackSamples.store (numSamples, std::memory_order_release);
            }
            double sum = 0.0;
            uint64_t crossings = 0;
            for (int i = 0; i < numSamples; ++i)
            {
                const double sample = (double) input[0][i];
                sum += sample * sample;
                if (lastSample <= 0.0 && sample > 0.0)
                    ++crossings;
                lastSample = sample;
            }
            energy.fetch_add (sum);
            frames.fetch_add (numSamples);
            rising.fetch_add (crossings);
            if (samples.size() < (size_t) (sampleRate * 10.0))
                samples.insert (samples.end(), input[0], input[0] + numSamples);
        }
        void audioDeviceAboutToStart (juce::AudioIODevice* device) override
        {
            sampleRate = device != nullptr ? device->getCurrentSampleRate() : 48000.0;
            if (sampleRate <= 0.0) sampleRate = 48000.0;
            startMs.store (juce::Time::getMillisecondCounterHiRes(), std::memory_order_release);
            samples.reserve ((size_t) (sampleRate * 10.0 + 4096.0));
        }
        void audioDeviceStopped() override {}

        std::atomic<double> energy { 0.0 };
        std::atomic<double> startMs { -1.0 };
        std::atomic<double> firstCallbackMs { -1.0 };
        std::atomic<int> firstCallbackSamples { 0 };
        std::atomic<bool> firstCallbackSeen { false };
        std::atomic<uint64_t> frames { 0 };
        std::atomic<uint64_t> rising { 0 };
        std::vector<float> samples;
        double sampleRate = 48000.0;
        double lastSample = 0.0;
    };

    struct LatencyOutput final : public juce::AudioIODeviceCallback
    {
        void audioDeviceIOCallbackWithContext (const float* const*, int,
                                               float* const* output, int numOutputs,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            for (int ch = 0; ch < numOutputs; ++ch)
            {
                if (output[ch] == nullptr) continue;
                for (int i = 0; i < numSamples; ++i)
                {
                    const int index = frames + i;
                    float value = 0.0f;
                    if (index >= clickFrame && index < clickFrame + clickLength)
                    {
                        const double t = (double) (index - clickFrame) / sampleRate;
                        value = (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * t)
                                         * std::exp (-t * 80.0));
                        if (! clickTimestampSet.exchange (true))
                            clickTimestampMs.store (juce::Time::getMillisecondCounterHiRes());
                    }
                    output[ch][i] = value;
                }
            }
            frames += numSamples;
        }
        void audioDeviceAboutToStart (juce::AudioIODevice* d) override
        {
            sampleRate = d != nullptr ? d->getCurrentSampleRate() : 48000.0;
            if (sampleRate <= 0.0) sampleRate = 48000.0;
            clickFrame = (int) (sampleRate * 0.15);
            clickLength = (int) (sampleRate * 0.02);
        }
        void audioDeviceStopped() override {}
        double sampleRate = 48000.0;
        int frames = 0, clickFrame = 7200, clickLength = 960;
        std::atomic<bool> clickTimestampSet { false };
        std::atomic<double> clickTimestampMs { -1.0 };
    };

    struct LatencyInput final : public juce::AudioIODeviceCallback
    {
        void audioDeviceIOCallbackWithContext (const float* const* input, int numInputs,
                                               float* const*, int, int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            if (numInputs <= 0 || input[0] == nullptr) return;
            for (int i = 0; i < numSamples; ++i)
            {
                const float magnitude = std::abs (input[0][i]);
                if (magnitude > peak.load())
                    peak.store (magnitude);
                if (magnitude > 0.02f && ! detected.exchange (true))
                {
                    detectedSample.store (frames + i);
                    detectedMs.store (juce::Time::getMillisecondCounterHiRes());
                }
            }
            frames += numSamples;
        }
        void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
        void audioDeviceStopped() override {}
        int frames = 0;
        std::atomic<bool> detected { false };
        std::atomic<int> detectedSample { -1 };
        std::atomic<double> detectedMs { -1.0 };
        std::atomic<float> peak { 0.0f };
    };

    int latency (const Options& o, juce::AudioIODeviceType& type)
    {
        type.scanForDevices();
        const auto inputName = findDeviceName (type.getDeviceNames (true), o.input);
        const auto outputName = findDeviceName (type.getDeviceNames (false), o.output);
        if (inputName.isEmpty() || outputName.isEmpty())
        {
            std::cerr << "Latency input/output not found: " << o.input << " / " << o.output << "\n";
            return 2;
        }
        auto inputDevice = std::unique_ptr<juce::AudioIODevice> (type.createDevice ({}, inputName));
        auto outputDevice = std::unique_ptr<juce::AudioIODevice> (type.createDevice (outputName, {}));
        if (inputDevice == nullptr || outputDevice == nullptr)
        {
            std::cerr << "Cannot create latency devices: input="
                      << (inputDevice ? "ok" : "null") << " output="
                      << (outputDevice ? "ok" : "null") << " names="
                      << inputName << " / " << outputName << "\n";
            return 3;
        }
        juce::BigInteger ins, outs;
        ins.setRange (0, juce::jmin (2, inputDevice->getInputChannelNames().size()), true);
        outs.setRange (0, juce::jmin (2, outputDevice->getOutputChannelNames().size()), true);
        LatencyInput inputCallback;
        LatencyOutput outputCallback;
        auto inputError = inputDevice->open (ins, {}, 48000.0, 512);
        auto outputError = outputDevice->open ({}, outs, 48000.0, 512);
        if (inputError.isNotEmpty() || outputError.isNotEmpty())
        {
            std::cerr << "Latency open failed: input=" << inputError << " output=" << outputError << "\n";
            return 4;
        }
        inputDevice->start (&inputCallback);
        outputDevice->start (&outputCallback);
        std::this_thread::sleep_for (std::chrono::duration<double> (o.seconds));
        inputDevice->stop();
        outputDevice->stop();
        inputDevice->close();
        outputDevice->close();
        const double click = outputCallback.clickTimestampMs.load();
        const double detect = inputCallback.detectedMs.load();
        std::cout << "Latency input=" << inputName << " output=" << outputName
                  << " click_ms=" << click << " detect_ms=" << detect
                  << " peak=" << inputCallback.peak.load()
                  << " input_samples=" << inputCallback.frames << "\n";
        if (click > 0.0 && detect > 0.0)
            std::cout << "LATENCY_MS " << (detect - click) << "\n";
        else
            std::cout << "LATENCY_MS unknown (impulse not detected)\n";
        return 0;
    }
    int play (const Options& o, juce::AudioIODeviceType& type)
    {
        type.scanForDevices();
        const auto name = findDeviceName (type.getDeviceNames (false), o.output);
        if (name.isEmpty())
        {
            std::cerr << "Output device not found: " << o.output << "\n";
            return 2;
        }
        auto device = std::unique_ptr<juce::AudioIODevice> (type.createDevice (name, {}));
        if (device == nullptr)
        {
            std::cerr << "Cannot create output device.\n";
            return 3;
        }
        juce::BigInteger none, outs;
        outs.setRange (0, juce::jmin (2, device->getOutputChannelNames().size()), true);
        std::unique_ptr<WavPlaybackCallback> wavCallback;
        ToneCallback toneCallback (o.toneHz);
        juce::AudioIODeviceCallback* callback = &toneCallback;
        if (o.wavPath.isNotEmpty())
        {
            wavCallback = std::make_unique<WavPlaybackCallback> (o.wavPath);
            callback = wavCallback.get();
            if (! wavCallback->reader)
            {
                std::cerr << "Cannot read WAV: " << o.wavPath << "\\n";
                return 8;
            }
        }
        const auto err = device->open (none, outs, 48000.0, 512);
        if (err.isNotEmpty())
        {
            std::cerr << "Open failed: " << err << "\n";
            return 4;
        }
        device->start (callback);
        std::this_thread::sleep_for (std::chrono::duration<double> (o.seconds));
        device->stop();
        device->close();
        const double startMs = o.wavPath.isNotEmpty() && wavCallback != nullptr
                                   ? wavCallback->startMs.load() : -1.0;
        if (o.wavPath.isNotEmpty())
            std::cout << "Played WAV " << o.wavPath << " for " << o.seconds
                      << " start_ms=" << startMs << "\n";
        else
            std::cout << "Played " << o.toneHz << " Hz for " << o.seconds
                  << " s to " << name << "\n";
        return 0;
    }

    int record (const Options& o, juce::AudioIODeviceType& type)
    {
        type.scanForDevices();
        const auto name = findDeviceName (type.getDeviceNames (true), o.input);
        if (name.isEmpty())
        {
            std::cerr << "Input device not found: " << o.input << "\n";
            return 2;
        }
        auto device = std::unique_ptr<juce::AudioIODevice> (type.createDevice ({}, name));
        if (device == nullptr)
        {
            std::cerr << "Cannot create input device.\n";
            return 3;
        }
        juce::BigInteger ins, none;
        ins.setRange (0, juce::jmin (2, device->getInputChannelNames().size()), true);
        CaptureCallback callback;
        const auto err = device->open (ins, none, 48000.0, 512);
        if (err.isNotEmpty())
        {
            std::cerr << "Open failed: " << err << "\n";
            return 4;
        }
        device->start (&callback);
        std::this_thread::sleep_for (std::chrono::duration<double> (o.seconds));
        device->stop();
        device->close();
        const auto frames = callback.frames.load();
        const double rms = frames > 0 ? std::sqrt (callback.energy.load() / (double) frames) : 0.0;
        const double db = rms > 1.0e-9 ? 20.0 * std::log10 (rms) : -100.0;
        const double seconds = frames > 0 ? (double) frames / callback.sampleRate : 0.0;
        const double frequency = seconds > 0.0 ? (double) callback.rising.load() / seconds : 0.0;

        double peakFrequency = 0.0;
        double peakMagnitude = 0.0;
        if (callback.samples.size() > 1024)
        {
            const int n = (int) juce::jmin<size_t> (callback.samples.size(),
                                                    (size_t) (callback.sampleRate * 2.0));
            for (double hz = 50.0; hz <= 5000.0; hz += 5.0)
            {
                const double omega = 2.0 * juce::MathConstants<double>::pi * hz / callback.sampleRate;
                double real = 0.0, imag = 0.0;
                for (int i = 0; i < n; ++i)
                {
                    const double angle = omega * i;
                    real += callback.samples[(size_t) i] * std::cos (angle);
                    imag -= callback.samples[(size_t) i] * std::sin (angle);
                }
                const double magnitude = real * real + imag * imag;
                if (magnitude > peakMagnitude)
                {
                    peakMagnitude = magnitude;
                    peakFrequency = hz;
                }
            }
        }

        if (o.outPath.isNotEmpty() && ! callback.samples.empty())
        {
            std::ofstream out (o.outPath.toRawUTF8(), std::ios::binary);
            if (out)
            {
                const uint32_t dataBytes = (uint32_t) (callback.samples.size() * 2);
                const uint32_t riffSize = 36 + dataBytes;
                const uint16_t channels = 1, bits = 16;
                const uint32_t rate = (uint32_t) callback.sampleRate;
                const uint32_t byteRate = rate * channels * bits / 8;
                const uint16_t blockAlign = channels * bits / 8;
                out.write ("RIFF", 4); out.write (reinterpret_cast<const char*> (&riffSize), 4);
                out.write ("WAVEfmt ", 8);
                const uint32_t fmtSize = 16; out.write (reinterpret_cast<const char*> (&fmtSize), 4);
                const uint16_t format = 1; out.write (reinterpret_cast<const char*> (&format), 2);
                out.write (reinterpret_cast<const char*> (&channels), 2);
                out.write (reinterpret_cast<const char*> (&rate), 4);
                out.write (reinterpret_cast<const char*> (&byteRate), 4);
                out.write (reinterpret_cast<const char*> (&blockAlign), 2);
                out.write (reinterpret_cast<const char*> (&bits), 2);
                out.write ("data", 4); out.write (reinterpret_cast<const char*> (&dataBytes), 4);
                for (float sample : callback.samples)
                {
                    const auto value = (int16_t) juce::jlimit (-32768.0f, 32767.0f,
                                                               sample * 32767.0f);
                    out.write (reinterpret_cast<const char*> (&value), 2);
                }
                std::cout << "Wrote capture WAV: " << o.outPath << " ("
                          << callback.samples.size() << " samples)\n";
            }
            else
                std::cerr << "Cannot write capture WAV: " << o.outPath << "\n";
        }
        std::cout << "Recorded " << name << " frames=" << frames
                  << " rms=" << rms << " dBFS=" << db
                  << " start_ms=" << callback.startMs.load()
                  << " first_cb_ms=" << callback.firstCallbackMs.load()
                  << " first_cb_samples=" << callback.firstCallbackSamples.load()
                  << " freq=" << frequency << " Hz"
                  << " peak=" << peakFrequency << " Hz\n";
        return 0;
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto o = parse (argc, argv);

    auto type = std::unique_ptr<juce::AudioIODeviceType> (
        juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared));
    if (type == nullptr)
    {
        std::cerr << "WASAPI unavailable.\n";
        return 1;
    }
    type->scanForDevices();

    if (o.list)
    {
        std::cout << "Inputs:\n";
        for (const auto& n : type->getDeviceNames (true)) std::cout << "  " << n << "\n";
        std::cout << "Outputs:\n";
        for (const auto& n : type->getDeviceNames (false)) std::cout << "  " << n << "\n";
        return 0;
    }

    if (argc < 2)
    {
        std::cout << "Usage: NLMatrixDriverAudioTest --play|--record|--latency [--input name] [--out file.wav] [--play-wav file.wav] "
                     "[--output name] [--seconds N] [--tone-hz N] [--list]\n";
        return 0;
    }

    if (o.wavPath.isNotEmpty())
        return play (o, *type);

    const std::string mode (argv[1]);
    if (mode == "--play") return play (o, *type);
    if (mode == "--latency") return latency (o, *type);
    if (mode == "--record") return record (o, *type);
    std::cout << "Unknown mode. Use --play, --record or --list.\n";
    return 0;
}
