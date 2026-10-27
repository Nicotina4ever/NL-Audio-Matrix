/*
    NLMatrixLive — first usable vertical slice of NL Audio Matrix.

    Modes:
      --wasapi-list
          list shared-mode WASAPI input/output endpoints (read-only).

      --live --input "<ASIO device>" --output "<WASAPI playback endpoint>"
             [--send <absChannel>:<gainDb>:<L|R|B>]...
             [--seconds N] [--mute] [--master-db X]

          Opens an ASIO input device (all channels), mixes selected channels to
          a stereo bus, and sends the bus to a WASAPI shared-mode output endpoint.
          No ASIO output is written.

      --live --testtone --output "<WASAPI endpoint>" [--seconds N]
          Generates a low-level 440 Hz sine into the same stereo bus. Useful to
          verify the output path when no ASIO source is available.

    The ring is a lock-free single-producer/single-consumer buffer. There is no
    sample-rate conversion in this slice: input and output must agree on the
    sample rate (the program refuses to run otherwise and says why).
*/

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <csignal>
#include <iostream>
#include <memory>
#include <vector>

namespace
{
    volatile std::sig_atomic_t gStopRequested = 0;

    void onSignal (int) { gStopRequested = 1; }

    //==============================================================================
    class StereoRing
    {
    public:
        explicit StereoRing (int capacityFrames)
            : capacity (capacityFrames), data ((size_t) capacityFrames * 2, 0.0f)
        {
            jassert ((capacity & (capacity - 1)) == 0);
        }

        int write (const float* left, const float* right, int numFrames)
        {
            const auto w = writePos.load (std::memory_order_relaxed);
            const auto r = readPos.load (std::memory_order_acquire);
            const int freeFrames = capacity - (int) (w - r);
            const int toWrite = juce::jmin (numFrames, freeFrames);

            for (int i = 0; i < toWrite; ++i)
            {
                const auto index = (size_t) ((w + (uint64_t) i) & (uint64_t) (capacity - 1));
                data[index * 2]     = left[i];
                data[index * 2 + 1] = right[i];
            }

            writePos.store (w + (uint64_t) toWrite, std::memory_order_release);
            return numFrames - toWrite;
        }

        int read (float* left, float* right, int numFrames)
        {
            const auto r = readPos.load (std::memory_order_relaxed);
            const auto w = writePos.load (std::memory_order_acquire);
            const int available = (int) (w - r);
            const int toRead = juce::jmin (numFrames, available);

            for (int i = 0; i < toRead; ++i)
            {
                const auto index = (size_t) ((r + (uint64_t) i) & (uint64_t) (capacity - 1));
                left[i]  = data[index * 2];
                right[i] = data[index * 2 + 1];
            }

            readPos.store (r + (uint64_t) toRead, std::memory_order_release);
            return toRead;
        }

        int available() const
        {
            const auto w = writePos.load (std::memory_order_acquire);
            const auto r = readPos.load (std::memory_order_acquire);
            return (int) (w - r);
        }

        void reset()
        {
            readPos.store (0, std::memory_order_relaxed);
            writePos.store (0, std::memory_order_relaxed);
        }

    private:
        const int capacity;
        std::vector<float> data;
        std::atomic<uint64_t> writePos { 0 };
        std::atomic<uint64_t> readPos { 0 };
    };

    //==============================================================================
    struct Send
    {
        int absoluteChannel = 0;
        float gainL = 0.0f;
        float gainR = 0.0f;
    };

    struct Stats
    {
        Stats()
        {
            for (auto& p : inputPeaks)
                p.store (0.0f, std::memory_order_relaxed);
        }

        std::array<std::atomic<float>, 64> inputPeaks;
        std::atomic<float> outputPeakL { 0.0f };
        std::atomic<float> outputPeakR { 0.0f };
        std::atomic<uint64_t> droppedFrames { 0 };
        std::atomic<uint64_t> underrunFrames { 0 };
        std::atomic<uint64_t> callbackCount { 0 };

        void resetPeaks()
        {
            for (auto& p : inputPeaks)
                p.store (0.0f, std::memory_order_relaxed);
            outputPeakL.store (0.0f, std::memory_order_relaxed);
            outputPeakR.store (0.0f, std::memory_order_relaxed);
        }
    };

    //==============================================================================
    class Matrix
    {
    public:
        void configure (const std::vector<int>& activeAbsoluteChannels, const std::vector<Send>& sends)
        {
            gainsL.assign (activeAbsoluteChannels.size(), 0.0f);
            gainsR.assign (activeAbsoluteChannels.size(), 0.0f);

            for (const auto& send : sends)
            {
                for (size_t i = 0; i < activeAbsoluteChannels.size(); ++i)
                {
                    if (activeAbsoluteChannels[i] == send.absoluteChannel)
                    {
                        gainsL[i] += send.gainL;
                        gainsR[i] += send.gainR;
                    }
                }
            }
        }

        void process (const float* const* input, int numInputChannels, int numSamples,
                      float* left, float* right, Stats& stats) const
        {
            const int channels = juce::jmin (numInputChannels, (int) gainsL.size());
            juce::FloatVectorOperations::clear (left, numSamples);
            juce::FloatVectorOperations::clear (right, numSamples);

            for (int ch = 0; ch < channels; ++ch)
            {
                const auto* src = input[ch];
                if (src == nullptr)
                    continue;

                const float gl = gainsL[(size_t) ch];
                const float gr = gainsR[(size_t) ch];
                if (gl == 0.0f && gr == 0.0f)
                    continue;

                float peak = 0.0f;
                for (int i = 0; i < numSamples; ++i)
                {
                    const float v = src[i];
                    left[i]  += v * gl;
                    right[i] += v * gr;
                    peak = juce::jmax (peak, std::abs (v));
                }

                auto& slot = stats.inputPeaks[(size_t) juce::jlimit (0, 63, ch)];
                if (peak > slot.load (std::memory_order_relaxed))
                    slot.store (peak, std::memory_order_relaxed);
            }

            for (int i = 0; i < numSamples; ++i)
            {
                left[i]  = juce::jlimit (-1.0f, 1.0f, left[i]);
                right[i] = juce::jlimit (-1.0f, 1.0f, right[i]);
            }
        }

    private:
        std::vector<float> gainsL, gainsR;
    };

    //==============================================================================
    class AsioProducer final : public juce::AudioIODeviceCallback
    {
    public:
        AsioProducer (StereoRing& r, Matrix& m, Stats& s) : ring (r), matrix (m), stats (s) {}

        void audioDeviceAboutToStart (juce::AudioIODevice* device) override
        {
            const int block = device != nullptr ? device->getCurrentBufferSizeSamples() : 512;
            scratchSize = juce::jmax (2048, block);
            mixL.assign ((size_t) scratchSize, 0.0f);
            mixR.assign ((size_t) scratchSize, 0.0f);
        }

        void audioDeviceStopped() override {}

        void audioDeviceError (const juce::String& message) override
        {
            lastError = message;
            error.store (true, std::memory_order_relaxed);
        }

        void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                               int numInputChannels,
                                               float* const* outputChannelData,
                                               int numOutputChannels,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (outputChannelData[ch] != nullptr)
                    juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

            const int n = juce::jmin (numSamples, scratchSize);
            if (n <= 0)
                return;

            matrix.process (inputChannelData, numInputChannels, n,
                            mixL.data(), mixR.data(), stats);

            const int dropped = ring.write (mixL.data(), mixR.data(), n);
            stats.droppedFrames.fetch_add ((uint64_t) dropped, std::memory_order_relaxed);
            stats.callbackCount.fetch_add (1, std::memory_order_relaxed);

            float pl = 0.0f, pr = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                pl = juce::jmax (pl, std::abs (mixL[(size_t) i]));
                pr = juce::jmax (pr, std::abs (mixR[(size_t) i]));
            }
            if (pl > stats.outputPeakL.load (std::memory_order_relaxed))
                stats.outputPeakL.store (pl, std::memory_order_relaxed);
            if (pr > stats.outputPeakR.load (std::memory_order_relaxed))
                stats.outputPeakR.store (pr, std::memory_order_relaxed);
        }

        std::atomic<bool> error { false };
        juce::String lastError;

    private:
        StereoRing& ring;
        Matrix& matrix;
        Stats& stats;
        std::vector<float> mixL, mixR;
        int scratchSize = 2048;
    };

    //==============================================================================
    class WasapiConsumer final : public juce::AudioIODeviceCallback
    {
    public:
        WasapiConsumer (StereoRing& r, Stats& s) : ring (r), stats (s) {}

        void audioDeviceAboutToStart (juce::AudioIODevice* device) override
        {
            const int block = device != nullptr ? device->getCurrentBufferSizeSamples() : 512;
            scratchSize = juce::jmax (2048, block);
            tmpL.assign ((size_t) scratchSize, 0.0f);
            tmpR.assign ((size_t) scratchSize, 0.0f);
        }

        void audioDeviceStopped() override {}

        void audioDeviceError (const juce::String& message) override
        {
            lastError = message;
            error.store (true, std::memory_order_relaxed);
        }

        void audioDeviceIOCallbackWithContext (const float* const*,
                                               int,
                                               float* const* outputChannelData,
                                               int numOutputChannels,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            const int n = juce::jmin (numSamples, scratchSize);
            const int got = n > 0 ? ring.read (tmpL.data(), tmpR.data(), n) : 0;

            if (got < n)
                stats.underrunFrames.fetch_add ((uint64_t) (n - got), std::memory_order_relaxed);

            float pl = 0.0f, pr = 0.0f;
            for (int i = 0; i < got; ++i)
            {
                pl = juce::jmax (pl, std::abs (tmpL[(size_t) i]));
                pr = juce::jmax (pr, std::abs (tmpR[(size_t) i]));
            }
            if (pl > stats.outputPeakL.load (std::memory_order_relaxed))
                stats.outputPeakL.store (pl, std::memory_order_relaxed);
            if (pr > stats.outputPeakR.load (std::memory_order_relaxed))
                stats.outputPeakR.store (pr, std::memory_order_relaxed);

            if (numOutputChannels > 0 && outputChannelData[0] != nullptr)
                std::copy (tmpL.begin(), tmpL.begin() + n, outputChannelData[0]);

            if (numOutputChannels > 1 && outputChannelData[1] != nullptr)
                std::copy (tmpR.begin(), tmpR.begin() + n, outputChannelData[1]);

            for (int ch = 2; ch < numOutputChannels; ++ch)
                if (outputChannelData[ch] != nullptr)
                    juce::FloatVectorOperations::clear (outputChannelData[ch], n);
        }

        std::atomic<bool> error { false };
        juce::String lastError;

    private:
        StereoRing& ring;
        Stats& stats;
        std::vector<float> tmpL, tmpR;
        int scratchSize = 2048;
    };

    //==============================================================================
    class TestToneProducer final : public juce::Thread
    {
    public:
        TestToneProducer (StereoRing& r, double rate) : juce::Thread ("testtone"), ring (r), sampleRate (rate) {}

        void run() override
        {
            constexpr int block = 128;
            std::vector<float> left ((size_t) block), right ((size_t) block);
            double phase = 0.0;
            const double increment = 2.0 * juce::MathConstants<double>::pi * 440.0 / sampleRate;
            double nextTime = juce::Time::getMillisecondCounterHiRes();

            while (! threadShouldExit())
            {
                for (int i = 0; i < block; ++i)
                {
                    const float sample = (float) (0.10 * std::sin (phase));
                    left[(size_t) i] = sample;
                    right[(size_t) i] = sample;
                    phase += increment;
                    if (phase >= juce::MathConstants<double>::twoPi)
                        phase -= juce::MathConstants<double>::twoPi;
                }

                ring.write (left.data(), right.data(), block);

                nextTime += 1000.0 * (double) block / sampleRate;
                const double now = juce::Time::getMillisecondCounterHiRes();
                const int sleepMs = (int) std::floor (nextTime - now);
                if (sleepMs > 0)
                    juce::Thread::sleep (sleepMs);
                else if (sleepMs < -100)
                    nextTime = now;
            }
        }

    private:
        StereoRing& ring;
        double sampleRate = 48000.0;
    };

    //==============================================================================
    struct Options
    {
        bool wasapiList = false;
        bool wasapiCapture = false;
        bool live = false;
        bool testTone = false;
        bool mute = false;
        juce::String input;
        juce::String output;
        juce::String captureInput;
        juce::String outFile;
        juce::StringArray sends;
        double seconds = 10.0;
        double masterDb = 0.0;
        bool help = false;
    };

    Options parseOptions (int argc, char* argv[])
    {
        Options o;
        juce::StringArray args;
        for (int i = 1; i < argc; ++i)
            args.add (juce::String (argv[i]));

        auto valueAfter = [&args] (const juce::String& key)
        {
            const int i = args.indexOf (key);
            return (i >= 0 && i + 1 < args.size()) ? args[i + 1] : juce::String();
        };

        o.wasapiList = args.contains ("--wasapi-list");
        o.wasapiCapture = args.contains ("--wasapi-capture");
        o.live = args.contains ("--live");
        o.testTone = args.contains ("--testtone");
        o.mute = args.contains ("--mute");
        o.help = args.contains ("--help") || args.contains ("-h");
        o.input = valueAfter ("--input");
        o.output = valueAfter ("--output");
        o.captureInput = valueAfter ("--capture-input");
        o.outFile = valueAfter ("--out");
        o.seconds = valueAfter ("--seconds").getDoubleValue();
        o.masterDb = valueAfter ("--master-db").getDoubleValue();

        if (o.seconds <= 0.0)
            o.seconds = 0.0; // run until Ctrl+C

        for (int i = 0; i < args.size(); ++i)
            if (args[i] == "--send" && i + 1 < args.size())
                o.sends.add (args[i + 1]);

        return o;
    }

    void printHelp()
    {
        std::cout
            << "NLMatrixLive — ASIO -> stereo matrix -> WASAPI output\n\n"
            << "  --wasapi-list\n"
            << "  --wasapi-capture --capture-input \"<WASAPI input>\" [--seconds N] [--out file.wav]\n"
            << "  --live --input \"<ASIO device>\" --output \"<WASAPI output>\"\n"
            << "         [--send <absChannel>:<gainDb>:<L|R|B>]...\n"
            << "         [--seconds N] [--mute] [--master-db X]\n"
            << "  --live --testtone --output \"<WASAPI output>\" [--seconds N]\n\n"
            << "Examples:\n"
            << "  NLMatrixLive --wasapi-list\n"
            << "  NLMatrixLive --live --testtone --output \"CABLE Input\" --seconds 5\n"
            << "  NLMatrixLive --live --input \"ReaRoute ASIO (x64)\" --output \"CABLE Input\"\n"
            << "         --send 0:0:L --send 1:0:R --seconds 10\n\n"
            << "Input/output sample rates must match; this slice has no SRC yet.\n"
            << "No ASIO output is written. Use --testtone to verify the output path.\n";
    }

    std::unique_ptr<juce::AudioIODeviceType> createAsioType()
    {
        return std::unique_ptr<juce::AudioIODeviceType> (
            juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
    }

    std::unique_ptr<juce::AudioIODeviceType> createWasapiType()
    {
        return std::unique_ptr<juce::AudioIODeviceType> (
            juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared));
    }

    juce::String findName (const juce::StringArray& names, const juce::String& requested,
                           const juce::String& preferredContains);

    juce::String chooseAsioInput (const juce::StringArray& names, const juce::String& requested)
    {
        const auto exact = findName (names, requested, {});
        if (exact.isNotEmpty())
            return exact;

        for (const auto& preferred : { juce::String ("Universal Audio"),
                                       juce::String ("Apollo"),
                                       juce::String ("ReaRoute") })
            for (const auto& n : names)
                if (n.containsIgnoreCase (preferred))
                    return n;

        return names.isEmpty() ? juce::String() : names[0];
    }

    juce::String findName (const juce::StringArray& names, const juce::String& requested,
                           const juce::String& preferredContains)
    {
        if (requested.isNotEmpty())
        {
            for (const auto& n : names)
                if (n == requested)
                    return n;
            for (const auto& n : names)
                if (n.containsIgnoreCase (requested))
                    return n;
            return {};
        }

        for (const auto& n : names)
            if (n.containsIgnoreCase (preferredContains))
                return n;

        return names.isEmpty() ? juce::String() : names[0];
    }

    int listWasapi()
    {
        auto type = createWasapiType();
        if (type == nullptr)
        {
            std::cerr << "WASAPI unavailable.\n";
            return 1;
        }

        type->scanForDevices();
        const auto outputs = type->getDeviceNames (false);
        const auto inputs  = type->getDeviceNames (true);

        std::cout << "WASAPI outputs (" << outputs.size() << "):\n";
        for (int i = 0; i < outputs.size(); ++i)
            std::cout << "  [" << i << "] " << outputs[i] << "\n";

        std::cout << "WASAPI inputs (" << inputs.size() << "):\n";
        for (int i = 0; i < inputs.size(); ++i)
            std::cout << "  [" << i << "] " << inputs[i] << "\n";

        return 0;
    }

    class WasapiCaptureCallback final : public juce::AudioIODeviceCallback
    {
    public:
        WasapiCaptureCallback (juce::AudioBuffer<float>& destination, std::atomic<int>& writePosition)
            : dest (destination), position (writePosition) {}

        void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
        void audioDeviceStopped() override { stopped.store (true, std::memory_order_relaxed); }
        void audioDeviceError (const juce::String&) override { error.store (true, std::memory_order_relaxed); }

        void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                               int numInputChannels,
                                               float* const* outputChannelData,
                                               int numOutputChannels,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (outputChannelData[ch] != nullptr)
                    juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

            const int destChannels = dest.getNumChannels();
            const int destSamples = dest.getNumSamples();
            const int p = position.load (std::memory_order_relaxed);
            const int room = destSamples - p;
            const int n = juce::jmin (numSamples, room, numInputChannels > 0 ? numSamples : 0);

            if (n > 0)
                for (int ch = 0; ch < juce::jmin (numInputChannels, destChannels); ++ch)
                    if (inputChannelData[ch] != nullptr)
                        dest.copyFrom (ch, p, inputChannelData[ch], n);

            if (n < numSamples)
                overflow.store (true, std::memory_order_relaxed);
            position.store (p + juce::jmax (0, n), std::memory_order_release);
        }

        std::atomic<bool> error { false };
        std::atomic<bool> stopped { false };
        std::atomic<bool> overflow { false };

    private:
        juce::AudioBuffer<float>& dest;
        std::atomic<int>& position;
    };

    int runWasapiCapture (const Options& options)
    {
        auto type = createWasapiType();
        if (type == nullptr)
        {
            std::cerr << "WASAPI unavailable.\n";
            return 1;
        }

        type->scanForDevices();
        const auto inputs = type->getDeviceNames (true);
        const auto name = findName (inputs, options.captureInput, "CABLE Output");
        if (name.isEmpty())
        {
            std::cerr << "No WASAPI input device found. Use --wasapi-list.\n";
            return 2;
        }

        std::cout << "WASAPI capture: " << name << "\n";
        std::unique_ptr<juce::AudioIODevice> device (type->createDevice ({}, name));
        if (device == nullptr)
        {
            std::cerr << "Could not create WASAPI input device.\n";
            return 3;
        }

        const auto inNames = device->getInputChannelNames();
        if (inNames.isEmpty())
        {
            std::cerr << "WASAPI input has no channels (busy/disconnected?).\n";
            return 4;
        }

        juce::BigInteger inMask;
        inMask.setRange (0, inNames.size(), true);
        juce::BigInteger outMask;
        auto err = device->open (inMask, outMask, 48000.0, 512);
        if (err.isNotEmpty())
        {
            std::cerr << "Could not open WASAPI input: " << err << "\n";
            return 5;
        }

        const double rate = device->getCurrentSampleRate();
        const int buffer = device->getCurrentBufferSizeSamples();
        const auto activeMask = device->getActiveInputChannels();
        const int activeChannels = activeMask.countNumberOfSetBits();
        const double seconds = options.seconds > 0.0 ? options.seconds : 5.0;
        const int totalSamples = juce::jmax (buffer, (int) std::ceil (rate * seconds) + 8 * buffer);

        std::vector<int> absolute;
        for (int i = 0; i < inNames.size(); ++i)
            if (activeMask[i])
                absolute.push_back (i);

        if (activeChannels <= 0 || absolute.size() != (size_t) activeChannels)
        {
            std::cerr << "Unexpected active-channel mask.\n";
            device->close();
            return 6;
        }

        juce::AudioBuffer<float> recorded (activeChannels, totalSamples);
        recorded.clear();
        std::atomic<int> position { 0 };
        WasapiCaptureCallback callback (recorded, position);

        device->start (&callback);
        if (! device->isPlaying())
        {
            std::cerr << "WASAPI capture did not start.\n";
            device->close();
            return 7;
        }

        std::cout << "Capturing " << seconds << " s at " << rate << " Hz, "
                  << activeChannels << " channel(s)...\n";
        juce::Thread::sleep ((int) std::lround (seconds * 1000.0));

        device->stop();
        device->close();

        const int captured = juce::jlimit (0, totalSamples, position.load());
        const int samplesToWrite = juce::jmin (captured, (int) std::ceil (rate * seconds));
        if (samplesToWrite <= 0)
        {
            std::cerr << "No samples captured.\n";
            return 8;
        }

        const int sourceChannels = juce::jmin (2, activeChannels);
        juce::AudioBuffer<float> wavBuffer (2, samplesToWrite);
        for (int ch = 0; ch < sourceChannels; ++ch)
            wavBuffer.copyFrom (ch, 0, recorded, ch, 0, samplesToWrite);
        if (sourceChannels == 1)
            wavBuffer.copyFrom (1, 0, recorded, 0, 0, samplesToWrite);

        const auto outFile = options.outFile.isNotEmpty()
                                 ? juce::File (options.outFile)
                                 : juce::File::getCurrentWorkingDirectory()
                                       .getChildFile ("reports")
                                       .getChildFile ("MVP")
                                       .getChildFile ("wasapi_capture.wav");
        outFile.getParentDirectory().createDirectory();
        if (outFile.existsAsFile())
            outFile.deleteFile();

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = outFile.createOutputStream();
        bool wrote = false;
        if (stream != nullptr)
        {
            const auto writerOptions = juce::AudioFormatWriterOptions{}
                                           .withSampleRate (rate)
                                           .withNumChannels (2)
                                           .withBitsPerSample (32)
                                           .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
            auto writer = wav.createWriterFor (stream, writerOptions);
            if (writer != nullptr)
                wrote = writer->writeFromAudioSampleBuffer (wavBuffer, 0, samplesToWrite);
        }

        float peak = 0.0f;
        double sum = 0.0;
        int crossings = 0;
        const auto* data = recorded.getReadPointer (0);
        for (int i = 0; i < samplesToWrite; ++i)
        {
            const float v = data[i];
            peak = juce::jmax (peak, std::abs (v));
            sum += (double) v * (double) v;
            if (i > 0 && data[i - 1] <= 0.0f && v > 0.0f)
                ++crossings;
        }

        const double rms = std::sqrt (sum / (double) samplesToWrite);
        const double approxHz = (double) crossings / seconds;
        std::cout << "Captured " << samplesToWrite << " samples, peak "
                  << juce::String (juce::Decibels::gainToDecibels (peak, -100.0f), 1)
                  << " dB, rms " << juce::String (juce::Decibels::gainToDecibels ((float) rms, -100.0f), 1)
                  << " dB, approx " << juce::String (approxHz, 0) << " Hz\n";
        std::cout << (wrote ? "WAV: " : "WAV write FAILED: ") << outFile.getFullPathName() << "\n";

        if (callback.error.load())
            std::cerr << "The WASAPI input reported an error.\n";
        return wrote ? 0 : 9;
    }

    bool parseSends (const juce::StringArray& textSends, const std::vector<int>& activeAbsolute,
                     std::vector<Send>& sends, double masterDb)
    {
        const float master = juce::Decibels::decibelsToGain ((float) masterDb);

        if (textSends.isEmpty())
        {
            if (activeAbsolute.size() >= 2)
            {
                sends.push_back ({ activeAbsolute[0], master, 0.0f });
                sends.push_back ({ activeAbsolute[1], 0.0f, master });
            }
            else if (activeAbsolute.size() == 1)
            {
                sends.push_back ({ activeAbsolute[0], master, master });
            }
            return ! sends.empty();
        }

        for (const auto& text : textSends)
        {
            juce::StringArray parts;
            parts.addTokens (text, ":", "");
            if (parts.size() < 2)
            {
                std::cerr << "Invalid --send '" << text << "' (expected channel:gain[:L|R|B]).\n";
                return false;
            }

            Send s;
            s.absoluteChannel = parts[0].getIntValue();
            const float gain = juce::Decibels::decibelsToGain ((float) parts[1].getDoubleValue()) * master;
            const juce::String target = parts.size() > 2 ? parts[2].toUpperCase() : juce::String ("B");

            if (target == "L")      { s.gainL = gain; }
            else if (target == "R") { s.gainR = gain; }
            else                     { s.gainL = s.gainR = gain; }

            const bool exists = std::find (activeAbsolute.begin(), activeAbsolute.end(),
                                           s.absoluteChannel) != activeAbsolute.end();
            if (! exists)
                std::cerr << "Warning: channel " << s.absoluteChannel << " is not active; send ignored.\n";
            else
                sends.push_back (s);
        }

        return ! sends.empty();
    }

    struct RunningDevices
    {
        std::unique_ptr<juce::AudioIODeviceType> asioType;
        std::unique_ptr<juce::AudioIODeviceType> wasapiType;
        std::unique_ptr<juce::AudioIODevice> asioDevice;
        std::unique_ptr<juce::AudioIODevice> outputDevice;
        std::unique_ptr<TestToneProducer> tone;
    };

    int runLive (const Options& options)
    {
        RunningDevices devices;
        StereoRing ring (1 << 16); // 65536 frames, power of two
        Matrix matrix;
        Stats stats;
        std::vector<Send> sends;
        std::vector<int> activeAbsolute;
        double sourceRate = 48000.0;

        std::unique_ptr<juce::AudioIODevice> inputDevice;
        juce::String inputName;

        if (! options.testTone)
        {
            devices.asioType = createAsioType();
            if (devices.asioType == nullptr)
            {
                std::cerr << "ASIO unavailable.\n";
                return 1;
            }

            devices.asioType->scanForDevices();
            const auto names = devices.asioType->getDeviceNames (false);
            inputName = chooseAsioInput (names, options.input);
            if (inputName.isEmpty())
            {
                std::cerr << "No ASIO input device found.\n";
                return 2;
            }

            std::cout << "ASIO input: " << inputName << "\n";
            inputDevice.reset (devices.asioType->createDevice (inputName, inputName));
            if (inputDevice == nullptr)
            {
                std::cerr << "Could not create ASIO device.\n";
                return 3;
            }

            const auto inNames = inputDevice->getInputChannelNames();
            if (inNames.isEmpty())
            {
                std::cerr << "The selected ASIO device exposes no input channels. "
                             "It may be disconnected, busy or uninitialized; no cause is assumed.\n";
                return 4;
            }

            juce::BigInteger inMask;
            inMask.setRange (0, inNames.size(), true);
            juce::BigInteger outMask;
            auto err = inputDevice->open (inMask, outMask, 48000.0, 256);
            if (err.isNotEmpty())
            {
                std::cout << "Input-only open failed: " << err << "; retrying with silent outputs.\n";
                outMask.setRange (0, inputDevice->getOutputChannelNames().size(), true);
                err = inputDevice->open (inMask, outMask, 48000.0, 256);
            }
            if (err.isNotEmpty())
            {
                std::cerr << "Could not open ASIO input: " << err << "\n";
                return 5;
            }

            sourceRate = inputDevice->getCurrentSampleRate();
            const auto activeMask = inputDevice->getActiveInputChannels();
            for (int i = 0; i < inNames.size(); ++i)
                if (activeMask[i])
                    activeAbsolute.push_back (i);

            if (activeAbsolute.empty())
            {
                std::cerr << "No active ASIO input channels.\n";
                return 6;
            }

            std::cout << "ASIO rate: " << sourceRate << " Hz, buffer "
                      << inputDevice->getCurrentBufferSizeSamples() << ", channels:";
            for (auto ch : activeAbsolute)
                std::cout << " [" << ch << "] " << inNames[ch];
            std::cout << "\n";
        }

        devices.wasapiType = createWasapiType();
        if (devices.wasapiType == nullptr)
        {
            std::cerr << "WASAPI unavailable.\n";
            return 7;
        }

        devices.wasapiType->scanForDevices();
        const auto outputNames = devices.wasapiType->getDeviceNames (false);
        const auto outputName = findName (outputNames, options.output, "CABLE Input");
        if (outputName.isEmpty())
        {
            std::cerr << "No WASAPI output device found. Use --wasapi-list.\n";
            return 8;
        }

        std::cout << "WASAPI output: " << outputName << "\n";
        devices.outputDevice.reset (devices.wasapiType->createDevice (outputName, {}));
        if (devices.outputDevice == nullptr)
        {
            std::cerr << "Could not create WASAPI output device.\n";
            return 9;
        }

        const auto outNames = devices.outputDevice->getOutputChannelNames();
        if (outNames.isEmpty())
        {
            std::cerr << "WASAPI output has no output channels.\n";
            return 10;
        }

        juce::BigInteger outMask;
        outMask.setRange (0, juce::jmin (2, outNames.size()), true);
        juce::BigInteger noInputs;

        auto outError = devices.outputDevice->open (noInputs, outMask, sourceRate,
                                                    options.testTone ? 512 : 256);
        if (outError.isNotEmpty())
        {
            std::cerr << "WASAPI open failed: " << outError << "\n";
            return 11;
        }

        const double outputRate = devices.outputDevice->getCurrentSampleRate();
        if (std::abs (outputRate - sourceRate) > 0.5)
        {
            std::cerr << "Sample-rate mismatch: input " << sourceRate
                      << " Hz, output " << outputRate
                      << " Hz. This slice has no SRC; choose an output at the same rate.\n";
            devices.outputDevice->close();
            return 12;
        }

        if (options.testTone)
        {
            sourceRate = outputRate;
            std::cout << "Test tone: 440 Hz, -20 dBFS, " << sourceRate << " Hz\n";
        }

        if (! options.testTone)
        {
            if (! parseSends (options.sends, activeAbsolute, sends, options.masterDb))
            {
                std::cerr << "No valid sends configured.\n";
                devices.outputDevice->close();
                return 13;
            }

            matrix.configure (activeAbsolute, sends);
            if (options.mute)
                matrix.configure (activeAbsolute, {});
        }

        AsioProducer asioProducer (ring, matrix, stats);
        WasapiConsumer wasapiConsumer (ring, stats);

        devices.outputDevice->start (&wasapiConsumer);
        if (! devices.outputDevice->isPlaying())
        {
            std::cerr << "WASAPI output did not start.\n";
            devices.outputDevice->close();
            return 14;
        }

        if (! options.testTone)
        {
            inputDevice->start (&asioProducer);
            if (! inputDevice->isPlaying())
            {
                std::cerr << "ASIO input did not start.\n";
                devices.outputDevice->stop();
                devices.outputDevice->close();
                return 15;
            }
        }
        else
        {
            devices.tone = std::make_unique<TestToneProducer> (ring, outputRate);
            devices.tone->startThread (juce::Thread::Priority::high);
        }

        std::cout << "Running. " << (options.seconds > 0.0 ? "Press Ctrl+C to stop early." : "Ctrl+C to stop.") << "\n";

        const double startTime = juce::Time::getMillisecondCounterHiRes();
        double nextMeter = startTime + 1000.0;

        while (! gStopRequested)
        {
            juce::Thread::sleep (50);

            const double now = juce::Time::getMillisecondCounterHiRes();
            if (options.seconds > 0.0 && now - startTime >= options.seconds * 1000.0)
                break;

            if (now >= nextMeter)
            {
                nextMeter += 1000.0;
                const float pl = stats.outputPeakL.exchange (0.0f);
                const float pr = stats.outputPeakR.exchange (0.0f);
                std::cout << "fill " << ring.available() << " fr"
                          << ", outL " << juce::String (juce::Decibels::gainToDecibels (pl, -100.0f), 1)
                          << " dB, outR " << juce::String (juce::Decibels::gainToDecibels (pr, -100.0f), 1)
                          << " dB, underrun " << stats.underrunFrames.load()
                          << ", dropped " << stats.droppedFrames.load()
                          << ", callbacks " << stats.callbackCount.load();

                if (! options.testTone)
                {
                    std::cout << ", in";
                    const int show = juce::jmin (8, (int) activeAbsolute.size());
                    for (int ch = 0; ch < show; ++ch)
                    {
                        const float inPeak = stats.inputPeaks[(size_t) ch].exchange (0.0f);
                        std::cout << " [" << activeAbsolute[(size_t) ch] << "] "
                                  << juce::String (juce::Decibels::gainToDecibels (inPeak, -100.0f), 1);
                    }
                }

                std::cout << "\n";
            }
        }

        if (devices.tone != nullptr)
        {
            devices.tone->signalThreadShouldExit();
            devices.tone->stopThread (2000);
            devices.tone.reset();
        }

        if (inputDevice != nullptr)
        {
            inputDevice->stop();
            inputDevice->close();
        }

        devices.outputDevice->stop();
        devices.outputDevice->close();

        std::cout << "Stopped.\n";
        return 0;
    }
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    std::signal (SIGINT, onSignal);

    const auto options = parseOptions (argc, argv);

    if (options.help)
    {
        printHelp();
        return 0;
    }

    if (options.wasapiList)
        return listWasapi();

    if (options.wasapiCapture)
        return runWasapiCapture (options);

    if (options.live)
        return runLive (options);

    printHelp();
    return 0;
}
