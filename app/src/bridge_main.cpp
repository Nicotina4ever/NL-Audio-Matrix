/*
    NLMatrixBridge — multi-bridge, bidirectional Windows <-> ASIO patchbay.

    One ASIO device is opened once (all inputs, outputs only when needed) and
    any number of independent bridges are attached to it:

        --bridge "name|win-to-asio|<WASAPI capture endpoint>|3,4"
            captures a Windows endpoint and writes it to ASIO output channels.
            This is the SessionLinkPRO/browser -> Apollo Virtual path.

        --bridge "name|asio-to-win|5,6|<WASAPI render endpoint>"
            copies ASIO input channels to a Windows render endpoint; a Windows
            application sees the corresponding capture endpoint as a microphone.

    Each bridge has its own stereo ring, so signals are not mixed unless the
    user explicitly maps two bridges to the same ASIO output channel (rejected)
    or the target device itself mixes them.

    Channel numbers are 0-based JUCE/ASIO buffer indices. Use --inspect-asio to
    print the real channel names and indices before configuring a bridge.

    No SRC in this slice. ASIO and all WASAPI endpoints must use the same rate.
    Existing Windows endpoints are used; creating new named Windows endpoints
    requires the virtual driver phase.
*/

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include "wasapi_loopback.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <iostream>
#include <memory>
#include <set>
#include <vector>

namespace
{
    volatile std::sig_atomic_t gStop = 0;
    void onSignal (int) { gStop = 1; }

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
            return (int) (writePos.load (std::memory_order_acquire)
                          - readPos.load (std::memory_order_acquire));
        }

    private:
        const int capacity;
        std::vector<float> data;
        std::atomic<uint64_t> writePos { 0 };
        std::atomic<uint64_t> readPos { 0 };
    };

    //==============================================================================
    struct Stats
    {
        std::atomic<float> asioOutPeakL { 0.0f };
        std::atomic<float> asioOutPeakR { 0.0f };
        std::atomic<float> winOutPeakL { 0.0f };
        std::atomic<float> winOutPeakR { 0.0f };
        std::atomic<uint64_t> dropped { 0 };
        std::atomic<uint64_t> underrun { 0 };
        std::atomic<uint64_t> callbacks { 0 };
    };

    struct BridgeDef
    {
        juce::String name;
        bool winToAsio = true;   // source -> ASIO output
        bool loopback = false;   // source is a render endpoint captured via WASAPI loopback
        juce::String endpoint;
        std::vector<int> channels; // 0-based ASIO buffer indices
    };

    struct Options
    {
        bool list = false;
        bool inspectAsio = false;
        bool help = false;
        juce::String asio;
        juce::String inspectDevice;
        juce::StringArray bridgeSpecs;
        double seconds = 0.0;
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

        o.list = args.contains ("--list");
        o.inspectAsio = args.contains ("--inspect-asio");
        o.help = args.contains ("--help") || args.contains ("-h");
        o.asio = valueAfter ("--asio");
        o.inspectDevice = valueAfter ("--inspect-asio");
        o.seconds = valueAfter ("--seconds").getDoubleValue();
        if (o.seconds < 0.0)
            o.seconds = 0.0;

        for (int i = 0; i < args.size(); ++i)
            if (args[i] == "--bridge" && i + 1 < args.size())
                o.bridgeSpecs.add (args[i + 1]);

        return o;
    }

    void printHelp()
    {
        std::cout
            << "NLMatrixBridge — multi-bridge Windows <-> ASIO patchbay\n\n"
            << "  --list\n"
            << "  --inspect-asio \"<ASIO device>\"\n"
            << "  --asio \"<ASIO device>\" --bridge \"<name>|win-to-asio|<win capture>|<ch0>[,<ch1>]\"\n"
            << "                                    --bridge \"<name>|loopback-to-asio|<win render>|<ch0>[,<ch1>]\"\n"
            << "                                    --bridge \"<name>|asio-to-win|<ch0>[,<ch1>]|<win render>\"\n"
            << "  [--seconds N]\n\n"
            << "Examples (Apollo: verify channel indices with --inspect-asio):\n"
            << "  NLMatrixBridge --asio \"Universal Audio USB\" \\\n"
            << "    --bridge \"SessionLinkPRO->Virtual 1/2|win-to-asio|CABLE Output (VB-Audio Virtual Cable)|2,3\" \\\n"
            << "    --bridge \"Virtual 1/2->Windows mic|asio-to-win|4,5|CABLE Input (VB-Audio Virtual Cable)\"\n\n"
            << "Examples (test without Apollo):\n"
            << "  NLMatrixBridge --asio \"ReaRoute ASIO (x64)\" \\\n"
            << "    --bridge \"test in|win-to-asio|CABLE Output (VB-Audio Virtual Cable)|0,1\"\n\n"
            << "Notes:\n"
            << "  * Channels are 0-based buffer indices; --inspect-asio prints names.\n"
            << "  * Repeat --bridge for as many independent mappings as needed.\n"
            << "  * ASIO outputs are written only by win-to-asio bridges; other outputs\n"
            << "    are zeroed. With Apollo, check UAD Console/REAPER routing first.\n"
            << "  * Reusing the same ASIO output channel in two bridges is rejected.\n"
            << "  * Reusing the same Windows endpoint in two bridges is rejected.\n"
            << "  * No SRC: ASIO and all WASAPI endpoints must share the sample rate.\n"
            << "  * This program does not create new Windows devices; it uses existing\n"
            << "    endpoints (VB-CABLE, VB-Audio Matrix, physical, later our driver).\n";
    }

    std::vector<int> parseChannels (const juce::String& text)
    {
        std::vector<int> result;
        juce::StringArray parts;
        parts.addTokens (text, ",", "");
        for (const auto& p : parts)
            if (p.trim().isNotEmpty())
                result.push_back (p.trim().getIntValue());
        return result;
    }

    std::unique_ptr<juce::AudioIODeviceType> asioType()
    {
        return std::unique_ptr<juce::AudioIODeviceType> (
            juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
    }

    std::unique_ptr<juce::AudioIODeviceType> wasapiType()
    {
        return std::unique_ptr<juce::AudioIODeviceType> (
            juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared));
    }

    juce::String chooseName (const juce::StringArray& names, const juce::String& requested,
                             const juce::String& fallbackContains)
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
            if (n.containsIgnoreCase (fallbackContains))
                return n;

        return names.isEmpty() ? juce::String() : names[0];
    }

    int listEverything()
    {
        if (auto at = asioType())
        {
            at->scanForDevices();
            const auto names = at->getDeviceNames (false);
            std::cout << "ASIO devices (" << names.size() << "):\n";
            for (int i = 0; i < names.size(); ++i)
                std::cout << "  [" << i << "] " << names[i] << "\n";
        }

        if (auto wt = wasapiType())
        {
            wt->scanForDevices();
            const auto outs = wt->getDeviceNames (false);
            const auto ins  = wt->getDeviceNames (true);
            std::cout << "\nWASAPI render endpoints (" << outs.size() << "):\n";
            for (int i = 0; i < outs.size(); ++i)
                std::cout << "  [" << i << "] " << outs[i] << "\n";
            std::cout << "\nWASAPI capture endpoints (" << ins.size() << "):\n";
            for (int i = 0; i < ins.size(); ++i)
                std::cout << "  [" << i << "] " << ins[i] << "\n";
        }
        return 0;
    }

    int inspectAsio (const juce::String& requested)
    {
        auto at = asioType();
        if (at == nullptr)
        {
            std::cerr << "ASIO unavailable.\n";
            return 1;
        }

        at->scanForDevices();
        const auto names = at->getDeviceNames (false);
        const auto name = chooseName (names, requested, "Universal Audio");
        if (name.isEmpty())
        {
            std::cerr << "No ASIO device found.\n";
            return 2;
        }

        std::cout << "ASIO device: " << name << "\n";
        std::unique_ptr<juce::AudioIODevice> device (at->createDevice (name, name));
        if (device == nullptr)
        {
            std::cerr << "Could not create ASIO device.\n";
            return 3;
        }

        const auto ins = device->getInputChannelNames();
        const auto outs = device->getOutputChannelNames();
        std::cout << "Inputs (" << ins.size() << "):\n";
        for (int i = 0; i < ins.size(); ++i)
            std::cout << "  [" << i << "] " << ins[i] << "\n";
        std::cout << "Outputs (" << outs.size() << "):\n";
        for (int i = 0; i < outs.size(); ++i)
            std::cout << "  [" << i << "] " << outs[i] << "\n";
        return 0;
    }

    //==============================================================================
    class WinInputCallback final : public juce::AudioIODeviceCallback
    {
    public:
        WinInputCallback (StereoRing& r, Stats& s) : ring (r), stats (s) {}

        void audioDeviceAboutToStart (juce::AudioIODevice* device) override
        {
            const int block = device != nullptr ? device->getCurrentBufferSizeSamples() : 512;
            tmpL.assign ((size_t) juce::jmax (2048, block), 0.0f);
            tmpR.assign ((size_t) juce::jmax (2048, block), 0.0f);
        }

        void audioDeviceStopped() override {}
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

            const int n = juce::jmin (numSamples, (int) tmpL.size());
            for (int i = 0; i < n; ++i)
            {
                tmpL[(size_t) i] = numInputChannels > 0 && inputChannelData[0] != nullptr
                                       ? inputChannelData[0][i] : 0.0f;
                tmpR[(size_t) i] = numInputChannels > 1 && inputChannelData[1] != nullptr
                                       ? inputChannelData[1][i]
                                       : tmpL[(size_t) i];
            }

            const int dropped = ring.write (tmpL.data(), tmpR.data(), n);
            stats.dropped.fetch_add ((uint64_t) dropped, std::memory_order_relaxed);
        }

        std::atomic<bool> error { false };

    private:
        StereoRing& ring;
        Stats& stats;
        std::vector<float> tmpL, tmpR;
    };

    class WinOutputCallback final : public juce::AudioIODeviceCallback
    {
    public:
        WinOutputCallback (StereoRing& r, Stats& s) : ring (r), stats (s) {}

        void audioDeviceAboutToStart (juce::AudioIODevice* device) override
        {
            const int block = device != nullptr ? device->getCurrentBufferSizeSamples() : 512;
            tmpL.assign ((size_t) juce::jmax (2048, block), 0.0f);
            tmpR.assign ((size_t) juce::jmax (2048, block), 0.0f);
        }

        void audioDeviceStopped() override {}
        void audioDeviceError (const juce::String&) override { error.store (true, std::memory_order_relaxed); }

        void audioDeviceIOCallbackWithContext (const float* const*,
                                               int,
                                               float* const* outputChannelData,
                                               int numOutputChannels,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            const int n = juce::jmin (numSamples, (int) tmpL.size());
            std::fill (tmpL.begin(), tmpL.begin() + n, 0.0f);
            std::fill (tmpR.begin(), tmpR.begin() + n, 0.0f);

            const int got = ring.read (tmpL.data(), tmpR.data(), n);
            if (got < n)
                stats.underrun.fetch_add ((uint64_t) (n - got), std::memory_order_relaxed);

            float pl = 0.0f, pr = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                pl = juce::jmax (pl, std::abs (tmpL[(size_t) i]));
                pr = juce::jmax (pr, std::abs (tmpR[(size_t) i]));
            }
            if (pl > stats.winOutPeakL.load (std::memory_order_relaxed))
                stats.winOutPeakL.store (pl, std::memory_order_relaxed);
            if (pr > stats.winOutPeakR.load (std::memory_order_relaxed))
                stats.winOutPeakR.store (pr, std::memory_order_relaxed);

            if (numOutputChannels > 0 && outputChannelData[0] != nullptr)
                std::copy (tmpL.begin(), tmpL.begin() + n, outputChannelData[0]);
            if (numOutputChannels > 1 && outputChannelData[1] != nullptr)
                std::copy (tmpR.begin(), tmpR.begin() + n, outputChannelData[1]);
            for (int ch = 2; ch < numOutputChannels; ++ch)
                if (outputChannelData[ch] != nullptr)
                    juce::FloatVectorOperations::clear (outputChannelData[ch], n);
        }

        std::atomic<bool> error { false };

    private:
        StereoRing& ring;
        Stats& stats;
        std::vector<float> tmpL, tmpR;
    };

    //==============================================================================
    struct BridgeRuntime
    {
        BridgeDef def;
        std::unique_ptr<StereoRing> ring;
        Stats stats;
        std::unique_ptr<juce::AudioIODevice> winDevice;
        std::unique_ptr<WasapiLoopbackCapture> loopback;
        std::unique_ptr<WinInputCallback> inCallback;
        std::unique_ptr<WinOutputCallback> outCallback;
    };

    class AsioBridgeCallback final : public juce::AudioIODeviceCallback
    {
    public:
        AsioBridgeCallback (std::vector<BridgeRuntime*> bridges,
                            const std::vector<int>& inputCompact,
                            const std::vector<int>& outputCompact,
                            Stats& aggregateStats)
            : runtimes (std::move (bridges)), inMap (inputCompact), outMap (outputCompact),
              stats (aggregateStats) {}

        void audioDeviceAboutToStart (juce::AudioIODevice* device) override
        {
            const int block = device != nullptr ? device->getCurrentBufferSizeSamples() : 256;
            scratchSize = juce::jmax (2048, block);
            tmpL.assign ((size_t) scratchSize, 0.0f);
            tmpR.assign ((size_t) scratchSize, 0.0f);
        }

        void audioDeviceStopped() override {}
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

            const int n = juce::jmin (numSamples, scratchSize);
            if (n <= 0)
                return;

            // Windows -> ASIO
            for (auto* rt : runtimes)
            {
                if (! rt->def.winToAsio)
                    continue;

                std::fill (tmpL.begin(), tmpL.begin() + n, 0.0f);
                std::fill (tmpR.begin(), tmpR.begin() + n, 0.0f);
                rt->ring->read (tmpL.data(), tmpR.data(), n);

                for (int c = 0; c < (int) rt->def.channels.size() && c < 2; ++c)
                {
                    const int absolute = rt->def.channels[(size_t) c];
                    if (absolute < 0 || absolute >= (int) outMap.size())
                        continue;
                    const int compact = outMap[(size_t) absolute];
                    if (compact < 0 || compact >= numOutputChannels || outputChannelData[compact] == nullptr)
                        continue;
                    const auto* src = c == 0 ? tmpL.data() : tmpR.data();
                    std::copy (src, src + n, outputChannelData[compact]);
                }

                float peak = 0.0f;
                for (int i = 0; i < n; ++i)
                    peak = juce::jmax (peak, std::abs (tmpL[(size_t) i]));
                if (peak > rt->stats.asioOutPeakL.load (std::memory_order_relaxed))
                    rt->stats.asioOutPeakL.store (peak, std::memory_order_relaxed);
            }

            // ASIO -> Windows
            for (auto* rt : runtimes)
            {
                if (rt->def.winToAsio)
                    continue;

                for (int i = 0; i < n; ++i)
                {
                    const int a0 = rt->def.channels.empty() ? -1 : rt->def.channels[0];
                    const int a1 = rt->def.channels.size() > 1 ? rt->def.channels[1] : a0;
                    const int c0 = (a0 >= 0 && a0 < (int) inMap.size()) ? inMap[(size_t) a0] : -1;
                    const int c1 = (a1 >= 0 && a1 < (int) inMap.size()) ? inMap[(size_t) a1] : -1;

                    tmpL[(size_t) i] = (c0 >= 0 && c0 < numInputChannels && inputChannelData[c0] != nullptr)
                                           ? inputChannelData[c0][i] : 0.0f;
                    tmpR[(size_t) i] = (c1 >= 0 && c1 < numInputChannels && inputChannelData[c1] != nullptr)
                                           ? inputChannelData[c1][i] : tmpL[(size_t) i];
                }

                const int dropped = rt->ring->write (tmpL.data(), tmpR.data(), n);
                rt->stats.dropped.fetch_add ((uint64_t) dropped, std::memory_order_relaxed);
            }

            stats.callbacks.fetch_add (1, std::memory_order_relaxed);
        }

        std::atomic<bool> error { false };

    private:
        std::vector<BridgeRuntime*> runtimes;
        std::vector<int> inMap, outMap;
        Stats& stats;
        std::vector<float> tmpL, tmpR;
        int scratchSize = 2048;
    };

    //==============================================================================
    std::unique_ptr<BridgeRuntime> createRuntime (const BridgeDef& def,
                                                  std::unique_ptr<juce::AudioIODevice> winDevice)
    {
        auto rt = std::make_unique<BridgeRuntime>();
        rt->def = def;
        rt->winDevice = std::move (winDevice);
        rt->ring = std::make_unique<StereoRing> (1 << 16);
        if (def.winToAsio)
            rt->inCallback = std::make_unique<WinInputCallback> (*rt->ring, rt->stats);
        else
            rt->outCallback = std::make_unique<WinOutputCallback> (*rt->ring, rt->stats);
        return rt;
    }

    int runBridge (const Options& options)
    {
        if (options.bridgeSpecs.isEmpty())
        {
            std::cerr << "No --bridge specified. Use --help for the syntax.\n";
            return 1;
        }

        std::vector<BridgeDef> defs;
        std::set<int> usedAsioOutputs;
        std::set<juce::String> usedEndpoints;

        for (const auto& spec : options.bridgeSpecs)
        {
            juce::StringArray parts;
            parts.addTokens (spec, "|", "");
            for (auto& p : parts)
                p = p.trim();

            if (parts.size() != 4)
            {
                std::cerr << "Invalid --bridge '" << spec
                          << "' (expected name|direction|endpoint|channels).\n";
                return 2;
            }

            BridgeDef def;
            def.name = parts[0];
            const auto direction = parts[1].toLowerCase();
            def.winToAsio = (direction == "win-to-asio" || direction == "w2a");
            def.loopback = (direction == "loopback-to-asio" || direction == "l2a");
            const bool asioToWin = (direction == "asio-to-win" || direction == "a2w");
            if (! def.winToAsio && ! def.loopback && ! asioToWin)
            {
                std::cerr << "Invalid direction '" << parts[1] << "' in bridge '" << def.name << "'.\n";
                return 3;
            }

            if (def.winToAsio || def.loopback)
            {
                def.winToAsio = true;
                def.endpoint = parts[2];
                def.channels = parseChannels (parts[3]);
            }
            else
            {
                def.channels = parseChannels (parts[2]);
                def.endpoint = parts[3];
            }

            if (def.channels.empty() || def.channels.size() > 2)
            {
                std::cerr << "Bridge '" << def.name << "' needs one or two ASIO channels.\n";
                return 4;
            }

            if (! def.winToAsio)
            {
                // validate later against ASIO input count
            }
            else
            {
                for (int ch : def.channels)
                {
                    if (! usedAsioOutputs.insert (ch).second)
                    {
                        std::cerr << "ASIO output channel " << ch
                                  << " is used by more than one bridge; refusing implicit mixing.\n";
                        return 6;
                    }
                }
            }

            if (! usedEndpoints.insert (def.endpoint).second)
            {
                std::cerr << "Windows endpoint '" << def.endpoint
                          << "' is used by more than one bridge; open it once and route explicitly.\n";
                return 7;
            }

            defs.push_back (def);
        }

        std::cout << "Bridge configuration:\n";
        for (const auto& d : defs)
        {
            std::cout << "  " << d.name << ": "
                      << (d.loopback ? "loopback->asio" : (d.winToAsio ? "win->asio" : "asio->win"))
                      << ", endpoint \"" << d.endpoint << "\", channels ";
            for (size_t i = 0; i < d.channels.size(); ++i)
                std::cout << (i ? "," : "") << d.channels[i];
            std::cout << "\n";
        }

        // ---- ASIO device -----------------------------------------------------
        auto at = asioType();
        if (at == nullptr)
        {
            std::cerr << "ASIO unavailable.\n";
            return 8;
        }
        at->scanForDevices();
        const auto asioNames = at->getDeviceNames (false);
        const auto asioName = chooseName (asioNames, options.asio, "Universal Audio");
        if (asioName.isEmpty())
        {
            std::cerr << "No ASIO device found.\n";
            return 9;
        }

        std::cout << "ASIO device: " << asioName << "\n";
        std::unique_ptr<juce::AudioIODevice> asioDevice (at->createDevice (asioName, asioName));
        if (asioDevice == nullptr)
        {
            std::cerr << "Could not create ASIO device.\n";
            return 10;
        }

        const auto asioInputs = asioDevice->getInputChannelNames();
        const auto asioOutputs = asioDevice->getOutputChannelNames();
        const bool needOutputs = std::any_of (defs.begin(), defs.end(),
                                              [] (const BridgeDef& d) { return d.winToAsio; });

        for (const auto& d : defs)
        {
            for (int ch : d.channels)
            {
                const int limit = d.winToAsio ? asioOutputs.size() : asioInputs.size();
                if (ch < 0 || ch >= limit)
                {
                    std::cerr << "Bridge '" << d.name << "': ASIO channel " << ch
                              << " out of range (0.." << limit - 1 << ").\n";
                    return 11;
                }
            }
        }

        juce::BigInteger asioInMask;
        asioInMask.setRange (0, asioInputs.size(), true);
        juce::BigInteger asioOutMask;
        if (needOutputs)
            asioOutMask.setRange (0, asioOutputs.size(), true);

        auto asioError = asioDevice->open (asioInMask, asioOutMask, 48000.0, 256);
        if (asioError.isNotEmpty() && ! needOutputs)
        {
            asioOutMask.setRange (0, asioOutputs.size(), true);
            asioError = asioDevice->open (asioInMask, asioOutMask, 48000.0, 256);
        }
        if (asioError.isNotEmpty())
        {
            std::cerr << "Could not open ASIO device: " << asioError << "\n";
            return 12;
        }

        const double asioRate = asioDevice->getCurrentSampleRate();
        std::cout << "ASIO rate: " << asioRate << " Hz, buffer "
                  << asioDevice->getCurrentBufferSizeSamples() << "\n";
        if (needOutputs)
            std::cout << "ASIO output writing enabled; check UAD Console/REAPER target.\n";

        // Map absolute ASIO channel indices to compact callback indices.
        const auto activeIn = asioDevice->getActiveInputChannels();
        const auto activeOut = asioDevice->getActiveOutputChannels();
        std::vector<int> inMap ((size_t) asioInputs.size(), -1), outMap ((size_t) asioOutputs.size(), -1);
        int compact = 0;
        for (int i = 0; i < asioInputs.size(); ++i)
            if (activeIn[i]) inMap[(size_t) i] = compact++;
        compact = 0;
        for (int i = 0; i < asioOutputs.size(); ++i)
            if (activeOut[i]) outMap[(size_t) i] = compact++;

        // ---- WASAPI endpoints ------------------------------------------------
        auto wt = wasapiType();
        if (wt == nullptr)
        {
            std::cerr << "WASAPI unavailable.\n";
            return 13;
        }
        wt->scanForDevices();
        const auto winIns = wt->getDeviceNames (true);
        const auto winOuts = wt->getDeviceNames (false);

        std::vector<std::unique_ptr<BridgeRuntime>> runtimes;
        for (const auto& d : defs)
        {
            const bool usesRenderEndpoint = d.loopback || ! d.winToAsio;
            const auto& names = usesRenderEndpoint ? winOuts : winIns;
            const auto resolved = chooseName (names, d.endpoint,
                                              usesRenderEndpoint ? "CABLE Input" : "CABLE Output");
            if (resolved.isEmpty())
            {
                std::cerr << "Windows endpoint not found for bridge '" << d.name << "'.\n";
                return 14;
            }

            std::cout << (d.loopback ? "Windows loopback: "
                                     : (d.winToAsio ? "Windows capture: " : "Windows render: "))
                      << resolved << " (" << d.name << ")\n";

            if (d.loopback)
            {
                auto rt = createRuntime (d, nullptr);
                rt->loopback = std::make_unique<WasapiLoopbackCapture>();
                auto* ring = rt->ring.get();
                auto* st = &rt->stats;
                std::string loopbackError;
                if (! rt->loopback->start (std::wstring (resolved.toWideCharPointer()),
                                           [ring, st] (const float* l, const float* r, int n)
                                           {
                                               const int dropped = ring->write (l, r, n);
                                               st->dropped.fetch_add ((uint64_t) dropped,
                                                                      std::memory_order_relaxed);
                                           },
                                           loopbackError))
                {
                    std::cerr << "Could not open WASAPI loopback for bridge '" << d.name
                              << "': " << loopbackError << "\n";
                    return 15;
                }

                if (std::abs (rt->loopback->getSampleRate() - asioRate) > 0.5)
                {
                    std::cerr << "Sample-rate mismatch on loopback endpoint for bridge '"
                              << d.name << "' (" << rt->loopback->getSampleRate()
                              << " Hz vs ASIO " << asioRate << " Hz).\n";
                    return 16;
                }

                std::cout << "  loopback rate " << rt->loopback->getSampleRate()
                          << " Hz, channels " << rt->loopback->getChannelCount() << "\n";
                runtimes.push_back (std::move (rt));
                continue;
            }

            std::unique_ptr<juce::AudioIODevice> winDevice;
            if (d.winToAsio)
            {
                winDevice.reset (wt->createDevice ({}, resolved));
                if (winDevice == nullptr)
                {
                    std::cerr << "Could not create Windows capture device.\n";
                    return 17;
                }
                const auto inNames = winDevice->getInputChannelNames();
                juce::BigInteger inMask;
                inMask.setRange (0, inNames.size(), true);
                juce::BigInteger noOut;
                const auto err = winDevice->open (inMask, noOut, asioRate, 512);
                if (err.isNotEmpty())
                {
                    std::cerr << "Could not open Windows capture: " << err << "\n";
                    return 18;
                }
            }
            else
            {
                winDevice.reset (wt->createDevice (resolved, {}));
                if (winDevice == nullptr)
                {
                    std::cerr << "Could not create Windows render device.\n";
                    return 19;
                }
                const auto outNames = winDevice->getOutputChannelNames();
                juce::BigInteger outMask;
                outMask.setRange (0, juce::jmin (2, outNames.size()), true);
                juce::BigInteger noIn;
                const auto err = winDevice->open (noIn, outMask, asioRate, 512);
                if (err.isNotEmpty())
                {
                    std::cerr << "Could not open Windows render: " << err << "\n";
                    return 20;
                }
            }

            if (std::abs (winDevice->getCurrentSampleRate() - asioRate) > 0.5)
            {
                std::cerr << "Sample-rate mismatch on Windows endpoint for bridge '" << d.name << "'.\n";
                return 21;
            }

            runtimes.push_back (createRuntime (d, std::move (winDevice)));
        }

        std::vector<BridgeRuntime*> runtimePtrs;
        runtimePtrs.reserve (runtimes.size());
        for (auto& rt : runtimes)
            runtimePtrs.push_back (rt.get());

        Stats aggregateStats;
        AsioBridgeCallback asioCallback (runtimePtrs, inMap, outMap, aggregateStats);

        // Start Windows endpoints, then ASIO. Native loopback captures are
        // already running from the open phase.
        for (auto& rt : runtimes)
        {
            if (rt->def.loopback || rt->winDevice == nullptr)
                continue;

            auto* cb = rt->def.winToAsio ? (juce::AudioIODeviceCallback*) rt->inCallback.get()
                                         : (juce::AudioIODeviceCallback*) rt->outCallback.get();
            rt->winDevice->start (cb);
            if (! rt->winDevice->isPlaying())
            {
                std::cerr << "Windows endpoint did not start for bridge '" << rt->def.name << "'.\n";
                return 22;
            }
        }

        asioDevice->start (&asioCallback);
        if (! asioDevice->isPlaying())
        {
            std::cerr << "ASIO device did not start.\n";
            return 21;
        }

        std::cout << "Running. Ctrl+C to stop.";
        if (options.seconds > 0.0)
            std::cout << " Auto-stop after " << options.seconds << " s.";
        std::cout << "\n";

        const double start = juce::Time::getMillisecondCounterHiRes();
        double nextMeter = start + 1000.0;

        while (! gStop)
        {
            juce::Thread::sleep (50);
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (options.seconds > 0.0 && now - start >= options.seconds * 1000.0)
                break;

            if (now >= nextMeter)
            {
                nextMeter += 1000.0;
                for (auto& rt : runtimes)
                {
                    if (rt->def.winToAsio)
                    {
                        std::cout << "[" << rt->def.name << "] fill " << rt->ring->available()
                                  << ", asioOut " << juce::String (juce::Decibels::gainToDecibels (
                                         rt->stats.asioOutPeakL.exchange (0.0f), -100.0f), 1)
                                  << " dB, dropped " << rt->stats.dropped.load();
                    }
                    else
                    {
                        std::cout << "[" << rt->def.name << "] fill " << rt->ring->available()
                                  << ", winOut " << juce::String (juce::Decibels::gainToDecibels (
                                         rt->stats.winOutPeakL.exchange (0.0f), -100.0f), 1)
                                  << " dB, underrun " << rt->stats.underrun.load()
                                  << ", dropped " << rt->stats.dropped.load();
                    }
                    std::cout << "\n";
                }
            }
        }

        asioDevice->stop();
        asioDevice->close();
        for (auto& rt : runtimes)
        {
            if (rt->loopback != nullptr)
                rt->loopback->stop();
            if (rt->winDevice != nullptr)
            {
                rt->winDevice->stop();
                rt->winDevice->close();
            }
        }

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

    if (options.list)
        return listEverything();

    if (options.inspectAsio)
        return inspectAsio (options.inspectDevice);

    return runBridge (options);
}
