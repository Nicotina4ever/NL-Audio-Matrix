/*
    NLMatrixProbe — minimal read/capture probe for NL Audio Matrix.

    Purpose (MVP/P1):
      --list                 enumerate ASIO devices (read-only, no driver open)
      --inspect [--device X] create the JUCE device and print channel names
      --capture [--device X] [--seconds N] [--out DIR]
                             open all inputs (outputs only if input-only fails),
                             capture every active input channel to its own WAV,
                             write a report with the channel<->buffer mapping.

    No output audio is ever written by design. If an output mask is required
    by a driver, its buffers are zeroed in the callback.

    This is a probe, not the final matrix. It uses JUCE 9 stock and does not
    include or link any OBS-ASIO code (GPL).
*/

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace
{
    struct CaptureCallback final : public juce::AudioIODeviceCallback
    {
        CaptureCallback (juce::AudioBuffer<float>& destination,
                         std::atomic<int>& writePosition,
                         std::atomic<bool>& overflowed)
            : dest (destination), position (writePosition), overflow (overflowed)
        {
        }

        void audioDeviceAboutToStart (juce::AudioIODevice* device) override
        {
            sampleRate = device != nullptr ? device->getCurrentSampleRate() : 0.0;
            blockSize  = device != nullptr ? device->getCurrentBufferSizeSamples() : 0;
        }

        void audioDeviceStopped() override {}

        void audioDeviceError (const juce::String&) override
        {
            deviceError.store (true, std::memory_order_relaxed);
        }

        void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                               int numInputChannels,
                                               float* const* outputChannelData,
                                               int numOutputChannels,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            // The probe never plays anything: keep all enabled outputs silent.
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (outputChannelData[ch] != nullptr)
                    juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

            const int destChannels = dest.getNumChannels();
            const int destSamples  = dest.getNumSamples();
            const int p            = position.load (std::memory_order_relaxed);
            const int room         = destSamples - p;
            const int n            = juce::jmin (numSamples, room);

            if (n > 0)
            {
                const int channelsToCopy = juce::jmin (numInputChannels, destChannels);
                for (int ch = 0; ch < channelsToCopy; ++ch)
                    if (inputChannelData[ch] != nullptr)
                        dest.copyFrom (ch, p, inputChannelData[ch], n);
            }

            if (n < numSamples)
                overflow.store (true, std::memory_order_relaxed);

            position.store (p + juce::jmax (0, n), std::memory_order_release);
        }

        std::atomic<bool> deviceError { false };

    private:
        juce::AudioBuffer<float>& dest;
        std::atomic<int>& position;
        std::atomic<bool>& overflow;
        double sampleRate = 0.0;
        int blockSize = 0;
    };

    struct Options
    {
        bool list = false;
        bool inspect = false;
        bool capture = false;
        juce::String device;
        juce::String outputDir;
        double seconds = 10.0;
        bool help = false;
    };

    void printHelp()
    {
        std::cout
            << "NLMatrixProbe - ASIO enumeration / capture probe (no output audio)\n\n"
            << "Usage:\n"
            << "  NLMatrixProbe --list\n"
            << "  NLMatrixProbe --inspect [--device \"<name>\"]\n"
            << "  NLMatrixProbe --capture [--device \"<name>\"] [--seconds N] [--out DIR]\n\n"
            << "Notes:\n"
            << "  --list     only reads the ASIO device list; it does not open a driver.\n"
            << "  --inspect  creates the JUCE device to read channel names; it may\n"
            << "             initialize the ASIO driver (use with REAPER closed).\n"
            << "  --capture  opens all inputs; outputs are opened only if the driver\n"
            << "             rejects input-only mode, and are kept silent.\n"
            << "  Default device: first name containing 'Universal Audio' or 'Apollo',\n"
            << "                  otherwise the first ASIO device.\n"
            << "  Default output: reports/MVP/capture\n";
    }

    Options parseOptions (int argc, char* argv[])
    {
        Options options;
        juce::StringArray args;
        for (int i = 1; i < argc; ++i)
            args.add (juce::String (argv[i]));

        if (args.isEmpty())
            options.list = true;

        auto valueAfter = [&args] (const juce::String& key) -> juce::String
        {
            const int index = args.indexOf (key);
            return index >= 0 && index + 1 < args.size() ? args[index + 1] : juce::String();
        };

        options.list     = args.contains ("--list") || args.isEmpty();
        options.inspect  = args.contains ("--inspect");
        options.capture  = args.contains ("--capture");
        options.help     = args.contains ("--help") || args.contains ("-h");
        options.device   = valueAfter ("--device");
        options.outputDir = valueAfter ("--out");
        options.seconds  = valueAfter ("--seconds").getDoubleValue();

        if (options.seconds <= 0.0 || options.seconds > 600.0)
            options.seconds = 10.0;

        return options;
    }

    std::unique_ptr<juce::AudioIODeviceType> createAsioType()
    {
        return std::unique_ptr<juce::AudioIODeviceType> (
            juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
    }

    juce::String chooseDeviceName (const juce::StringArray& names, const juce::String& requested)
    {
        if (requested.isNotEmpty())
            return requested;

        for (const auto& name : names)
            if (name.containsIgnoreCase ("Universal Audio") || name.containsIgnoreCase ("Apollo"))
                return name;

        return names.isEmpty() ? juce::String() : names[0];
    }

    void printChannelNames (juce::AudioIODevice& device)
    {
        const auto inputs  = device.getInputChannelNames();
        const auto outputs = device.getOutputChannelNames();

        std::cout << "  Input channels (" << inputs.size() << "):\n";
        for (int i = 0; i < inputs.size(); ++i)
            std::cout << "    [" << i << "] " << inputs[i] << "\n";

        std::cout << "  Output channels (" << outputs.size() << "):\n";
        for (int i = 0; i < outputs.size(); ++i)
            std::cout << "    [" << i << "] " << outputs[i] << "\n";
    }

    int doList (juce::AudioIODeviceType& type)
    {
        type.scanForDevices();
        const auto names = type.getDeviceNames();

        std::cout << "ASIO devices: " << names.size() << "\n";
        for (int i = 0; i < names.size(); ++i)
            std::cout << "  [" << i << "] " << names[i] << "\n";

        if (names.isEmpty())
        {
            std::cerr << "No ASIO devices found. Check the ASIO driver installation.\n";
            return 2;
        }

        return 0;
    }

    int doInspect (juce::AudioIODeviceType& type, const Options& options)
    {
        type.scanForDevices();
        const auto names = type.getDeviceNames();
        const auto name  = chooseDeviceName (names, options.device);

        if (name.isEmpty())
        {
            std::cerr << "No ASIO device available.\n";
            return 2;
        }

        std::cout << "Device: " << name << "\n";
        std::unique_ptr<juce::AudioIODevice> device (type.createDevice (name, name));
        if (device == nullptr)
        {
            std::cerr << "Could not create the JUCE ASIO device.\n";
            return 3;
        }

        printChannelNames (*device);

        if (device->getInputChannelNames().isEmpty() && device->getOutputChannelNames().isEmpty())
            std::cout << "  No channels: driver present, but hardware not connected/active or device busy.\n";

        std::cout << "  Current sample rate: " << device->getCurrentSampleRate() << "\n";
        std::cout << "  Current buffer: " << device->getCurrentBufferSizeSamples() << " samples\n";
        return 0;
    }

    bool writeChannelWav (const juce::File& file,
                          const juce::AudioBuffer<float>& source,
                          int channel,
                          int numSamples,
                          double sampleRate)
    {
        if (numSamples <= 0)
            return false;

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        if (stream == nullptr)
            return false;

        const auto writerOptions = juce::AudioFormatWriterOptions{}
                                       .withSampleRate (sampleRate)
                                       .withNumChannels (1)
                                       .withBitsPerSample (32)
                                       .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

        auto writer = wav.createWriterFor (stream, writerOptions);
        if (writer == nullptr)
            return false;

        juce::AudioBuffer<float> temp (1, numSamples);
        temp.copyFrom (0, 0, source, channel, 0, numSamples);
        return writer->writeFromAudioSampleBuffer (temp, 0, numSamples);
    }

    juce::String safeFilePart (const juce::String& text)
    {
        juce::String s = text.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_");
        return s.isEmpty() ? juce::String ("channel") : s;
    }

    int doCapture (juce::AudioIODeviceType& type, const Options& options)
    {
        type.scanForDevices();
        const auto names = type.getDeviceNames();
        const auto name  = chooseDeviceName (names, options.device);

        if (name.isEmpty())
        {
            std::cerr << "No ASIO device available.\n";
            return 2;
        }

        std::cout << "Device: " << name << "\n";
        std::unique_ptr<juce::AudioIODevice> device (type.createDevice (name, name));
        if (device == nullptr)
        {
            std::cerr << "Could not create the JUCE ASIO device.\n";
            return 3;
        }

        const auto allInputNames  = device->getInputChannelNames();
        const auto allOutputNames = device->getOutputChannelNames();

        if (allInputNames.isEmpty())
        {
            std::cerr << "The device exposes no input channels.\n";
            return 4;
        }

        printChannelNames (*device);

        juce::BigInteger inputMask;
        inputMask.setRange (0, allInputNames.size(), true);

        double sampleRate = 48000.0;
        const auto availableRates = device->getAvailableSampleRates();
        if (! availableRates.isEmpty() && ! availableRates.contains (sampleRate))
            sampleRate = availableRates[0];

        int bufferSize = 256;
        const auto availableBuffers = device->getAvailableBufferSizes();
        if (! availableBuffers.isEmpty() && ! availableBuffers.contains (bufferSize))
            bufferSize = availableBuffers[0];

        juce::BigInteger outputMask;
        auto openError = device->open (inputMask, outputMask, sampleRate, bufferSize);

        if (openError.isNotEmpty())
        {
            // Some ASIO drivers require at least one output buffer. Open all
            // outputs but keep them silent; never route audio to them.
            std::cout << "Input-only open failed: " << openError << "\n";
            std::cout << "Retrying with all outputs enabled (silent)...\n";
            outputMask.setRange (0, allOutputNames.size(), true);
            openError = device->open (inputMask, outputMask, sampleRate, bufferSize);
        }

        if (openError.isNotEmpty())
        {
            std::cerr << "Could not open the ASIO device: " << openError << "\n";
            std::cerr << "Close REAPER or other ASIO hosts, then retry. No driver was modified.\n";
            return 5;
        }

        const double actualRate  = device->getCurrentSampleRate();
        const int actualBuffer   = device->getCurrentBufferSizeSamples();
        const auto activeMask    = device->getActiveInputChannels();
        const int activeChannels = activeMask.countNumberOfSetBits();

        std::vector<int> absoluteChannels;
        for (int i = 0; i < allInputNames.size(); ++i)
            if (activeMask[i])
                absoluteChannels.push_back (i);

        if (activeChannels <= 0 || absoluteChannels.size() != (size_t) activeChannels)
        {
            std::cerr << "Unexpected active-channel mask.\n";
            device->close();
            return 6;
        }

        // Extra driver buffers absorb callback granularity; the report/WAV are
        // trimmed to the requested duration below.
        const int requestedSamples = (int) std::ceil (actualRate * options.seconds);
        const int totalSamples = juce::jmax (actualBuffer,
                                             requestedSamples + 8 * actualBuffer);

        juce::AudioBuffer<float> recorded (activeChannels, totalSamples);
        recorded.clear();

        std::atomic<int> position { 0 };
        std::atomic<bool> overflow { false };
        CaptureCallback callback (recorded, position, overflow);

        device->start (&callback);

        if (! device->isPlaying())
        {
            std::cerr << "The ASIO device did not start.\n";
            device->close();
            return 7;
        }

        std::cout << "Capturing " << options.seconds << " s at " << actualRate
                  << " Hz, buffer " << actualBuffer << ", " << activeChannels
                  << " active input channel(s)...\n";

        juce::Thread::sleep ((int) std::lround (options.seconds * 1000.0));

        device->stop();
        device->close();

        if (callback.deviceError.load())
            std::cerr << "The driver reported an error during capture.\n";
        // The safety buffer can fill on a long first callback burst; this is
        // only a problem if the requested duration was not fully captured.
        if (overflow.load() && position.load() < requestedSamples)
            std::cerr << "Capture buffer overflow: not all samples were retained.\n";

        const int captured = juce::jlimit (0, totalSamples, position.load());
        const int samplesToWrite = juce::jmin (captured, requestedSamples);
        if (samplesToWrite <= 0)
        {
            std::cerr << "No samples captured.\n";
            return 8;
        }

        juce::File outDir (options.outputDir.isNotEmpty()
                               ? options.outputDir
                               : juce::File::getCurrentWorkingDirectory()
                                     .getChildFile ("reports")
                                     .getChildFile ("MVP")
                                     .getChildFile ("capture"));
        const auto dirResult = outDir.createDirectory();
        if (! dirResult.wasOk())
        {
            std::cerr << "Could not create output directory: " << dirResult.getErrorMessage() << "\n";
            return 9;
        }

        juce::String report;
        report << "NL Audio Matrix - ASIO capture report\n";
        report << "date: " << juce::Time::getCurrentTime().toString (true, true) << "\n";
        report << "device: " << name << "\n";
        report << "sampleRate: " << actualRate << "\n";
        report << "bufferSize: " << actualBuffer << "\n";
        report << "capturedSamples: " << samplesToWrite << "\n";
        report << "activeInputChannels: " << activeChannels << "\n";
        report << "outputsOpened: " << (outputMask.countNumberOfSetBits() > 0 ? "yes (silent)" : "no") << "\n";
        report << "inputMask: " << inputMask.toString (2) << "\n";
        report << "activeMask: " << activeMask.toString (2) << "\n";
        report << "channelMap (compactIndex -> absoluteChannel, name, file, peakDb, rmsDb):\n";

        for (int ch = 0; ch < activeChannels; ++ch)
        {
            const int absolute = absoluteChannels[(size_t) ch];
            const juce::String displayName = allInputNames[absolute];
            const auto fileName = "ch" + juce::String (absolute).paddedLeft ('0', 2)
                                  + "_" + safeFilePart (displayName) + ".wav";
            const auto file = outDir.getChildFile (fileName);

            float peak = 0.0f;
            double sumSquares = 0.0;
            const auto* data = recorded.getReadPointer (ch);
            for (int i = 0; i < samplesToWrite; ++i)
            {
                const float v = data[i];
                peak = juce::jmax (peak, std::abs (v));
                sumSquares += (double) v * (double) v;
            }
            const double rms = samplesToWrite > 0 ? std::sqrt (sumSquares / (double) samplesToWrite) : 0.0;
            const float peakDb = juce::Decibels::gainToDecibels (peak, -100.0f);
            const float rmsDb  = juce::Decibels::gainToDecibels ((float) rms, -100.0f);

            const bool wrote = writeChannelWav (file, recorded, ch, samplesToWrite, actualRate);
            report << "  " << ch << " -> " << absolute << ", \"" << displayName << "\", "
                   << (wrote ? fileName : juce::String ("WRITE_FAILED"))
                   << ", peak " << juce::String (peakDb, 2) << " dB"
                   << ", rms " << juce::String (rmsDb, 2) << " dB\n";

            std::cout << "  ch " << ch << " -> absolute " << absolute << " \""
                      << displayName << "\" " << (wrote ? "written" : "FAILED")
                      << " peak " << juce::String (peakDb, 1) << " dB\n";
        }

        const auto reportFile = outDir.getChildFile ("capture_report.txt");
        reportFile.replaceWithText (report);
        std::cout << "Report: " << reportFile.getFullPathName() << "\n";
        std::cout << "WAV files: " << outDir.getFullPathName() << "\n";

        return 0;
    }
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        if (arg == "--help" || arg == "-h")
        {
            printHelp();
            return 0;
        }
    }

    const auto options = parseOptions (argc, argv);
    if (options.help)
    {
        printHelp();
        return 0;
    }

    auto type = createAsioType();
    if (type == nullptr)
    {
        std::cerr << "JUCE ASIO device type unavailable. Build with JUCE_ASIO=1.\n";
        return 1;
    }

    if (options.capture)
        return doCapture (*type, options);

    if (options.inspect)
        return doInspect (*type, options);

    return doList (*type);
}
