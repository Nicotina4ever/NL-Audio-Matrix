/*
    NLMatrixRouter — config-driven N×M audio patchbay.

    One ASIO device (full-duplex) plus any number of Windows endpoints.
    Sources can be:
      - asio-in  : one or two ASIO input channels
      - loopback : any WASAPI render endpoint, captured natively without a
                   capture endpoint such as CABLE Output
      - capture  : any WASAPI capture endpoint (microphone, virtual capture)

    Destinations can be:
      - asio-out : one or two ASIO output channels (REAPER/Apollo path)
      - render   : any WASAPI render endpoint (application return, monitor)

    Routes are explicit N×M entries. Mix-minus is expressed by simply not
    routing a source to its own return destination; no hidden assumptions.
    Configuration is a JSON preset, so different sessions can be saved as
    separate files.

    No VB-Audio/ReaRoute dependency: those can be used as endpoints, but any
    physical or third-party WASAPI endpoint works, and the loopback capture
    path does not require a virtual cable.
*/

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>

#include "wasapi_loopback.h"
#include "process_loopback.h"
#include "nl_driver_client.h"
#include "matrix_engine.h"
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>

#if JUCE_WINDOWS
 #include <timeapi.h>
 #pragma comment(lib, "winmm.lib")
#endif
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

namespace
{
    volatile std::sig_atomic_t gStop = 0;
    void onSignal (int) { gStop = 1; }

    constexpr int kRingFrames = 1 << 16;

    //==============================================================================
    class StereoRing
    {
    public:
        explicit StereoRing (int capacityFrames)
            : capacity (capacityFrames), data ((size_t) capacityFrames * 2, 0.0f)
        {
            jassert ((capacity & (capacity - 1)) == 0);
        }

        // Single-frame read used by the streaming rate converter.  The caller
        // is the sole reader, so this is still a plain SPSC operation.
        bool readFrame (float& left, float& right)
        {
            const auto r = readPos.load (std::memory_order_relaxed);
            const auto w = writePos.load (std::memory_order_acquire);
            if (r == w)
                return false;
            const auto index = (size_t) (r & (uint64_t) (capacity - 1));
            left = data[index * 2];
            right = data[index * 2 + 1];
            readPos.store (r + 1, std::memory_order_release);
            return true;
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
                data[index * 2] = left[i];
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
                left[i] = data[index * 2];
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
    // Streaming stereo sample-rate converter.  It reads a source ring clocked by
    // one device and produces frames for a destination clock, using linear
    // interpolation and a slow fill-level trim.  The trim only has authority of
    // +/-300 ppm, so it removes residual drift without audible pitch movement;
    // the declared sample rates carry the bulk of the conversion.
    struct StereoRateConverter
    {
        static constexpr double MAX_TRIM = 300.0e-6;

        void reset()
        {
            haveFrame = false;
            phase = 0.0;
            trim = 0.0;
            fade = 0.0f;
            previousL = previousR = currentL = currentR = 0.0f;
        }

        void read (StereoRing& ring, float* outL, float* outR, int numFrames,
                   double sourceRate, double destinationRate, int targetFrames)
        {
            const double baseRatio = sourceRate / destinationRate;
            if (! (baseRatio > 0.0) || numFrames <= 0)
            {
                std::fill (outL, outL + numFrames, 0.0f);
                std::fill (outR, outR + numFrames, 0.0f);
                return;
            }

            if (targetFrames > 0)
            {
                const double error = (double) (ring.available() - targetFrames)
                                     / (double) targetFrames;
                const double correction = juce::jlimit (-MAX_TRIM, MAX_TRIM, error * 0.0005);
                trim = trim * 0.995 + correction * 0.005;
            }
            const double ratio = baseRatio * (1.0 + trim);

            if (! haveFrame)
            {
                if (! ring.readFrame (currentL, currentR))
                {
                    std::fill (outL, outL + numFrames, 0.0f);
                    std::fill (outR, outR + numFrames, 0.0f);
                    return;
                }
                previousL = currentL;
                previousR = currentR;
                haveFrame = true;
                phase = 0.0;
                fade = 0.0f;
            }

            for (int i = 0; i < numFrames; ++i)
            {
                bool underrun = false;
                while (phase >= 1.0)
                {
                    previousL = currentL;
                    previousR = currentR;
                    if (! ring.readFrame (currentL, currentR))
                    {
                        underrun = true;
                        break;
                    }
                    phase -= 1.0;
                }

                if (underrun)
                {
                    // Leave the converter unprimed: the next call fills a whole
                    // block instead of replaying the last sample indefinitely.
                    std::fill (outL + i, outL + numFrames, 0.0f);
                    std::fill (outR + i, outR + numFrames, 0.0f);
                    previousL = previousR = currentL = currentR = 0.0f;
                    haveFrame = false;
                    phase = 0.0;
                    fade = 0.0f;
                    return;
                }

                const float t = (float) juce::jlimit (0.0, 1.0, phase);
                const float ramp = fade;
                fade = juce::jmin (1.0f, fade + 1.0f / 64.0f);
                outL[i] = (previousL + (currentL - previousL) * t) * ramp;
                outR[i] = (previousR + (currentR - previousR) * t) * ramp;
                phase += ratio;
            }
        }

        bool haveFrame = false;
        double phase = 0.0;
        double trim = 0.0;
        float fade = 0.0f;
        float previousL = 0.0f, previousR = 0.0f;
        float currentL = 0.0f, currentR = 0.0f;
    };

    //==============================================================================
    static float nlReadSample (const uint8_t* p, const NLMATRIX_STREAM_INFO& f)
    {
        if (f.IsFloat)
        {
            float v = 0.0f;
            std::memcpy (&v, p, sizeof (v));
            return v;
        }
        if (f.BitsPerSample == 16)
        {
            int16_t v = 0;
            std::memcpy (&v, p, sizeof (v));
            return (float) v / 32768.0f;
        }
        if (f.BitsPerSample == 24)
        {
            int32_t v = (int32_t) p[0] | ((int32_t) p[1] << 8) | ((int32_t) p[2] << 16);
            if ((v & 0x800000) != 0)
                v |= ~0xFFFFFF;
            return (float) v / 8388608.0f;
        }
        if (f.BitsPerSample == 32)
        {
            int32_t v = 0;
            std::memcpy (&v, p, sizeof (v));
            return (float) ((double) v / 2147483648.0);
        }
        return 0.0f;
    }

    static void nlWriteSample (uint8_t* p, const NLMATRIX_STREAM_INFO& f, float v)
    {
        if (f.IsFloat)
        {
            std::memcpy (p, &v, sizeof (v));
        }
        else if (f.BitsPerSample == 16)
        {
            const auto s = (int16_t) juce::jlimit (-32768.0f, 32767.0f, v * 32767.0f);
            std::memcpy (p, &s, sizeof (s));
        }
        else if (f.BitsPerSample == 24)
        {
            const auto s = (int32_t) juce::jlimit (-8388608.0f, 8388607.0f, v * 8388607.0f);
            p[0] = (uint8_t) (s & 0xff);
            p[1] = (uint8_t) ((s >> 8) & 0xff);
            p[2] = (uint8_t) ((s >> 16) & 0xff);
        }
        else if (f.BitsPerSample == 32)
        {
            const auto s = (int32_t) juce::jlimit (-2147483648.0, 2147483647.0, (double) v * 2147483647.0);
            std::memcpy (p, &s, sizeof (s));
        }
    }

    static int nlRawToStereo (const uint8_t* raw, int bytes, const NLMATRIX_STREAM_INFO& f,
                              float* left, float* right)
    {
        const int block = (int) f.BlockAlign;
        if (block <= 0 || bytes <= 0)
            return 0;
        const int frames = juce::jmin (bytes / block, 4096);
        const int bytesPerSample = (int) juce::jmax (1UL, f.BitsPerSample / 8);
        const int channels = (int) juce::jmax (1UL, f.Channels);
        for (int i = 0; i < frames; ++i)
        {
            const auto* frame = raw + (size_t) i * block;
            const float l = nlReadSample (frame, f);
            const float r = channels > 1 ? nlReadSample (frame + bytesPerSample, f) : l;
            left[i] = l;
            right[i] = r;
        }
        return frames;
    }

    static int nlStereoToRaw (const float* left, const float* right, int frames,
                              const NLMATRIX_STREAM_INFO& f, std::vector<uint8_t>& raw)
    {
        const int block = (int) f.BlockAlign;
        if (block <= 0 || frames <= 0)
            return 0;
        raw.resize ((size_t) frames * block);
        const int bytesPerSample = (int) juce::jmax (1UL, f.BitsPerSample / 8);
        const int channels = (int) juce::jmax (1UL, f.Channels);
        for (int i = 0; i < frames; ++i)
        {
            auto* frame = raw.data() + (size_t) i * block;
            nlWriteSample (frame, f, left[i]);
            for (int ch = 1; ch < channels; ++ch)
                nlWriteSample (frame + (size_t) ch * bytesPerSample, f,
                               ch == 1 ? right[i] : left[i]);
        }
        return frames;
    }

    enum class SourceType { AsioInput, Loopback, Capture, Driver };
    enum class DestinationType { AsioOutput, Render, Driver };

    struct DeviceDef
    {
        juce::String id, backend, driver, displayName;
        double sampleRate = 48000.0;
        int bufferSize = 0;
        bool clockMaster = false;
    };

    struct SourceDef
    {
        juce::String id, name, type, endpoint, busId, process, processTitle, deviceId;
        int pid = 0;
        int driverEndpoint = 0;
        bool includeProcessTree = true;
        bool enabled = true;
        bool muted = false;
        bool solo = false;
        float gainDb = 0.0f;
        float pan = 0.0f;
        bool polarity = false;
        double baseFrequency = 300.0;
        double testLevel = 0.2;
        std::vector<int> channels;
    };

    struct BusDef
    {
        juce::String id, name;
        bool enabled = true;
        bool muted = false;
        bool solo = false;
        float gainDb = 0.0f;
        int channels = 2;
    };

    struct DestinationDef
    {
        juce::String id, name, type, endpoint, deviceId;
        int driverEndpoint = 0;
        bool enabled = true;
        bool muted = false;
        bool solo = false;
        bool monitor = false;
        float gainDb = 0.0f;
        std::vector<int> channels;
        std::vector<juce::String> mixMinusSources;
    };

    struct RouteDef
    {
        juce::String sourceId, destinationId;
        float gainL = 1.0f;
        float gainR = 1.0f;
        std::vector<int> channelMap;
        bool enabled = true;
    };

    struct Config
    {
        int version = 2;
        juce::String name;
        juce::String asio;
        std::vector<DeviceDef> devices;
        std::vector<BusDef> buses;
        std::vector<SourceDef> sources;
        std::vector<DestinationDef> destinations;
        std::vector<RouteDef> routes;
    };

    static juce::String baseProcessName (const juce::String& path)
    {
        auto name = path.fromLastOccurrenceOf ("\\", false, false);
        if (name.isEmpty()) name = path;
        return name.trim().toLowerCase();
    }

    struct ProcessWindowSearch
    {
        const juce::String* executable = nullptr;
        const juce::String* title = nullptr;
        unsigned long pid = 0;
        bool exactTitleFound = false;
    };

    static BOOL CALLBACK findProcessWindow (HWND hwnd, LPARAM parameter)
    {
        auto* search = reinterpret_cast<ProcessWindowSearch*> (parameter);
        if (search == nullptr || search->executable == nullptr)
            return TRUE;
        const int titleLength = GetWindowTextLengthW (hwnd);
        if (titleLength <= 0)
            return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId (hwnd, &pid);
        if (pid == 0)
            return TRUE;
        HANDLE process = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process == nullptr)
            return TRUE;
        wchar_t path[32768] = {};
        DWORD pathLength = (DWORD) std::size (path);
        const bool gotPath = QueryFullProcessImageNameW (process, 0, path, &pathLength) != 0;
        CloseHandle (process);
        if (! gotPath)
            return TRUE;
        if (baseProcessName (juce::String (path, (int) pathLength)) != *search->executable)
            return TRUE;

        wchar_t windowTitle[1024] = {};
        GetWindowTextW (hwnd, windowTitle, (int) std::size (windowTitle));
        const juce::String title (windowTitle);
        if (search->title != nullptr && search->title->isNotEmpty())
        {
            if (! title.containsIgnoreCase (*search->title))
                return TRUE;
            search->pid = pid;
            search->exactTitleFound = true;
            return FALSE;
        }
        if (search->pid == 0)
            search->pid = pid;
        return TRUE;
    }

    static unsigned long findProcessIdForSource (const SourceDef& source)
    {
        if (source.process.isEmpty())
            return (unsigned long) juce::jmax (0, source.pid);
        const auto executable = baseProcessName (source.process);
        ProcessWindowSearch search;
        search.executable = &executable;
        search.title = &source.processTitle;
        EnumWindows (findProcessWindow, reinterpret_cast<LPARAM> (&search));
        if (search.pid != 0)
            return search.pid;
        unsigned long fallback = 0;
        if (ProcessLoopbackCapture::findProcessIdByName (source.process.toWideCharPointer(), fallback))
            return fallback;
        return 0;
    }
    juce::var prop (const juce::var& v, const char* key)
    {
        if (auto* object = v.getDynamicObject())
            return object->getProperty (juce::Identifier (key));
        return {};
    }

    juce::String stringProp (const juce::var& v, const char* key)
    {
        return prop (v, key).toString();
    }

    std::vector<int> channelProp (const juce::var& v)
    {
        std::vector<int> result;
        if (auto* array = prop (v, "channels").getArray())
            for (const auto& item : *array)
                result.push_back ((int) item);
        return result;
    }

    bool loadConfig (const juce::File& file, Config& config, juce::String& error)
    {
        if (! file.existsAsFile())
        {
            error = "config file not found: " + file.getFullPathName();
            return false;
        }

        const auto parsed = juce::JSON::parse (file);
        if (parsed.isVoid())
        {
            error = "invalid JSON";
            return false;
        }

        config.version = juce::jmax (2, (int) prop (parsed, "version").toString().getIntValue());
        config.name = stringProp (parsed, "name");
        config.asio = stringProp (parsed, "asio");
        if (auto* deviceArray = prop (parsed, "devices").getArray())
        {
            for (const auto& item : *deviceArray)
            {
                DeviceDef d;
                d.id = stringProp (item, "id");
                d.backend = stringProp (item, "backend").toLowerCase();
                d.driver = stringProp (item, "driver");
                d.displayName = stringProp (item, "displayName");
                d.sampleRate = prop (item, "sampleRate").toString().getDoubleValue();
                if (d.sampleRate <= 0.0) d.sampleRate = 48000.0;
                d.bufferSize = (int) prop (item, "buffer").toString().getIntValue();
                d.clockMaster = (bool) prop (item, "clockMaster");
                if (d.id.isEmpty() || d.backend.isEmpty())
                {
                    error = "device needs id and backend";
                    return false;
                }
                config.devices.push_back (d);
            }
        }
        if (config.asio.isEmpty() && config.devices.empty())
        {
            error = "missing \"asio\" device name";
            return false;
        }

        std::set<juce::String> ids;

        if (auto* array = prop (parsed, "buses").getArray())
        {
            for (const auto& item : *array)
            {
                BusDef b;
                b.id = stringProp (item, "id");
                b.name = stringProp (item, "name");
                if (b.id.isEmpty())
                {
                    error = "bus needs id";
                    return false;
                }
                if (! ids.insert (b.id).second)
                {
                    error = "duplicate id: " + b.id;
                    return false;
                }
                if (b.name.isEmpty())
                    b.name = b.id;
                const auto busEnabled = prop (item, "enabled");
                b.enabled = busEnabled.isVoid() ? true : (bool) busEnabled;
                b.gainDb = juce::jlimit (-120.0f, 24.0f, (float) prop (item, "gainDb").toString().getDoubleValue());
                b.solo = (bool) prop (item, "solo");
                b.muted = (bool) prop (item, "muted");
                const auto busChannels = prop (item, "channels");
                if (! busChannels.isVoid())
                    b.channels = juce::jlimit (1, 32, (int) busChannels.toString().getIntValue());
                config.buses.push_back (b);
            }
        }

        if (auto* array = prop (parsed, "sources").getArray())
        {
            for (const auto& item : *array)
            {
                SourceDef s;
                s.id = stringProp (item, "id");
                s.name = stringProp (item, "name");
                s.type = stringProp (item, "type").toLowerCase();
                s.endpoint = stringProp (item, "endpoint");
                s.busId = stringProp (item, "bus");
                s.process = stringProp (item, "process");
                s.processTitle = stringProp (item, "processTitle");
                s.driverEndpoint = (int) prop (item, "driverEndpoint").toString().getIntValue();
                s.pid = (int) prop (item, "pid").toString().getIntValue();
                const auto tree = prop (item, "includeProcessTree");
                s.includeProcessTree = tree.isVoid() ? true : (bool) tree;
                const auto sourceEnabled = prop (item, "enabled");
                s.enabled = sourceEnabled.isVoid() ? true : (bool) sourceEnabled;
                s.deviceId = stringProp (item, "deviceId");
                s.gainDb = juce::jlimit (-120.0f, 24.0f, (float) prop (item, "gainDb").toString().getDoubleValue());
                s.pan = juce::jlimit (-1.0f, 1.0f, (float) prop (item, "pan").toString().getDoubleValue());
                s.polarity = (bool) prop (item, "polarity");
                s.solo = (bool) prop (item, "solo");
                s.muted = (bool) prop (item, "muted");
                s.channels = channelProp (item);
                const auto testLevel = prop (item, "level");
                if (! testLevel.isVoid())
                    s.testLevel = juce::jlimit (0.0, 1.0, testLevel.toString().getDoubleValue());
                const auto baseFrequency = prop (item, "baseFrequency");
                if (! baseFrequency.isVoid())
                    s.baseFrequency = baseFrequency.toString().getDoubleValue();
                if (s.id.isEmpty() || s.type.isEmpty())
                {
                    error = "source needs id and type";
                    return false;
                }
                if (! ids.insert (s.id).second)
                {
                    error = "duplicate source id: " + s.id;
                    return false;
                }
                if (s.name.isEmpty())
                    s.name = s.id;
                if (s.type == "asio-in" && s.channels.empty())
                {
                    error = "asio-in source needs channels";
                    return false;
                }
                if ((s.type == "loopback" || s.type == "capture") && s.endpoint.isEmpty())
                {
                    error = "loopback/capture source needs endpoint";
                    return false;
                }
                if (s.type == "bus" && s.busId.isEmpty())
                    s.busId = s.id;
                if (s.type == "bus" && s.busId.isEmpty())
                {
                    error = "bus source needs bus id";
                    return false;
                }
                if (s.type == "process" && s.process.isEmpty() && s.pid <= 0)
                {
                    error = "process source needs process name or pid";
                    return false;
                }
                config.sources.push_back (s);
            }
        }

        if (auto* array = prop (parsed, "destinations").getArray())
        {
            for (const auto& item : *array)
            {
                DestinationDef d;
                d.id = stringProp (item, "id");
                d.name = stringProp (item, "name");
                d.type = stringProp (item, "type").toLowerCase();
                d.endpoint = stringProp (item, "endpoint");
                d.driverEndpoint = (int) prop (item, "driverEndpoint").toString().getIntValue();
                d.deviceId = stringProp (item, "deviceId");
                d.gainDb = juce::jlimit (-120.0f, 24.0f, (float) prop (item, "gainDb").toString().getDoubleValue());
                d.monitor = (bool) prop (item, "monitor");
                d.muted = (bool) prop (item, "muted");
                d.solo = (bool) prop (item, "solo");
                d.channels = channelProp (item);
                if (auto* exclude = prop (item, "mixMinus").getArray())
                    for (const auto& value : *exclude)
                        d.mixMinusSources.push_back (value.toString());
                const auto destinationEnabled = prop (item, "enabled");
                d.enabled = destinationEnabled.isVoid() ? true : (bool) destinationEnabled;
                if (d.id.isEmpty() || d.type.isEmpty())
                {
                    error = "destination needs id and type";
                    return false;
                }
                if (! ids.insert (d.id).second)
                {
                    error = "duplicate destination id: " + d.id;
                    return false;
                }
                if (d.name.isEmpty())
                    d.name = d.id;
                if (d.type == "asio-out" && d.channels.empty())
                {
                    error = "asio-out destination needs channels";
                    return false;
                }
                if (d.type == "render" && d.endpoint.isEmpty())
                {
                    error = "render destination needs endpoint";
                    return false;
                }
                if (d.type == "driver" && d.id.isEmpty())
                {
                    error = "driver destination needs id";
                    return false;
                }
                config.destinations.push_back (d);
            }
        }

        std::map<juce::String, int> sourceIndex;
        for (int i = 0; i < (int) config.sources.size(); ++i)
            sourceIndex[config.sources[(size_t) i].id] = i;
        std::map<juce::String, int> destinationIndex;
        for (int i = 0; i < (int) config.destinations.size(); ++i)
            destinationIndex[config.destinations[(size_t) i].id] = i;
        std::set<juce::String> busIds;
        for (const auto& b : config.buses)
            busIds.insert (b.id);

        if (auto* array = prop (parsed, "routes").getArray())
        {
            for (const auto& item : *array)
            {
                RouteDef r;
                r.sourceId = stringProp (item, "source");
                r.destinationId = stringProp (item, "destination");
                const auto gain = (float) prop (item, "gainDb").toString().getDoubleValue();
                r.gainL = r.gainR = juce::Decibels::decibelsToGain (gain);
                if (auto* map = prop (item, "channelMap").getArray())
                    for (const auto& value : *map)
                        r.channelMap.push_back ((int) value);
                const auto enabledVar = prop (item, "enabled");
                r.enabled = enabledVar.isVoid() ? true : (bool) enabledVar;
                if (! sourceIndex.count (r.sourceId))
                {
                    error = "route references unknown source: " + r.sourceId;
                    return false;
                }
                if (! destinationIndex.count (r.destinationId) && ! busIds.count (r.destinationId))
                {
                    error = "route references unknown destination: " + r.destinationId;
                    return false;
                }
                config.routes.push_back (r);
            }
        }

        // Zero routes is a valid live state: deleting the last cable must
        // update the running graph to silence, not be rejected as an invalid
        // profile (which would keep the previous audio path alive).
        return true;
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

    //==============================================================================
    class CaptureCallback final : public juce::AudioIODeviceCallback
    {
    public:
        CaptureCallback (nlmatrix::MatrixRing& r, std::atomic<float>& peak,
                         std::atomic<double>& rate)
            : ring (r), peakOut (peak), rateOut (rate)
        {
            const int n = ring.getChannels();
            tmp.assign ((size_t) n, {});
        }

        void audioDeviceAboutToStart (juce::AudioIODevice* device) override
        {
            const int block = device != nullptr ? device->getCurrentBufferSizeSamples() : 512;
            for (auto& channel : tmp)
                channel.assign ((size_t) juce::jmax (2048, block), 0.0f);
            if (device != nullptr && device->getCurrentSampleRate() > 0.0)
                rateOut.store (device->getCurrentSampleRate(), std::memory_order_release);
        }
        void audioDeviceStopped() override {}
        void audioDeviceError (const juce::String&) override { error.store (true); }

        void audioDeviceIOCallbackWithContext (const float* const* input, int numInputs,
                                               float* const* output, int numOutputs, int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            for (int ch = 0; ch < numOutputs; ++ch)
                if (output[ch] != nullptr)
                    juce::FloatVectorOperations::clear (output[ch], numSamples);

            const int n = juce::jmin (numSamples, (int) tmp.front().size());
            std::vector<const float*> pointers ((size_t) tmp.size(), nullptr);
            for (int ch = 0; ch < (int) tmp.size(); ++ch)
            {
                auto& channel = tmp[(size_t) ch];
                std::fill (channel.begin(), channel.begin() + n, 0.0f);
                if (ch < numInputs && input[ch] != nullptr)
                    std::copy (input[ch], input[ch] + n, channel.begin());
                pointers[(size_t) ch] = channel.data();
            }
            ring.write (pointers.data(), (int) pointers.size(), n);

            float peak = 0.0f;
            for (const auto& channel : tmp)
                for (int i = 0; i < n; ++i)
                    peak = juce::jmax (peak, std::abs (channel[(size_t) i]));
            if (peak > peakOut.load())
                peakOut.store (peak);
        }

        std::atomic<bool> error { false };

    private:
        nlmatrix::MatrixRing& ring;
        std::atomic<float>& peakOut;
        std::atomic<double>& rateOut;
        std::vector<std::vector<float>> tmp;
    };

    class RenderCallback final : public juce::AudioIODeviceCallback
    {
    public:
        RenderCallback (nlmatrix::MatrixRing& r, std::atomic<float>& peak, double graphRate)
            : ring (r), peakOut (peak), graphSampleRate (graphRate),
              converter (r.getChannels())
        {
            tmp.assign ((size_t) ring.getChannels(), {});
        }

        void audioDeviceAboutToStart (juce::AudioIODevice* device) override
        {
            const int block = device != nullptr ? device->getCurrentBufferSizeSamples() : 512;
            for (auto& channel : tmp)
                channel.assign ((size_t) juce::jmax (2048, block), 0.0f);
            deviceSampleRate = device != nullptr ? device->getCurrentSampleRate() : graphSampleRate;
            converter.reset (ring.getChannels());
        }
        void audioDeviceStopped() override {}
        void audioDeviceError (const juce::String&) override { error.store (true); }

        void audioDeviceIOCallbackWithContext (const float* const*, int,
                                               float* const* output, int numOutputs, int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            const int n = juce::jmin (numSamples, (int) tmp.front().size());
            std::vector<float*> pointers ((size_t) tmp.size(), nullptr);
            for (int ch = 0; ch < (int) tmp.size(); ++ch)
            {
                std::fill (tmp[(size_t) ch].begin(), tmp[(size_t) ch].begin() + n, 0.0f);
                pointers[(size_t) ch] = tmp[(size_t) ch].data();
            }
            if (std::abs (deviceSampleRate - graphSampleRate) <= 0.5)
                ring.read (pointers.data(), (int) pointers.size(), n);
            else
                converter.read (ring, pointers.data(), (int) pointers.size(), n,
                                graphSampleRate, deviceSampleRate, getTargetFrames());

            float peak = 0.0f;
            for (const auto& channel : tmp)
                for (int i = 0; i < n; ++i)
                    peak = juce::jmax (peak, std::abs (channel[(size_t) i]));
            if (peak > peakOut.load())
                peakOut.store (peak);

            for (int ch = 0; ch < numOutputs; ++ch)
            {
                if (output[ch] == nullptr)
                    continue;
                if (ch < (int) tmp.size())
                    std::copy (tmp[(size_t) ch].begin(), tmp[(size_t) ch].begin() + n, output[ch]);
                else
                    juce::FloatVectorOperations::clear (output[ch], n);
            }
        }

        std::atomic<bool> error { false };

    private:
        int getTargetFrames() const noexcept
        {
            return juce::jmax (256, (int) std::lround (deviceSampleRate * 0.015));
        }

        nlmatrix::MatrixRing& ring;
        std::atomic<float>& peakOut;
        const double graphSampleRate;
        double deviceSampleRate = 0.0;
        nlmatrix::MatrixRateConverter converter;
        std::vector<std::vector<float>> tmp;
    };

    //==============================================================================
    struct SourceRuntime
    {
        SourceDef def;
        std::unique_ptr<nlmatrix::MatrixRing> ring;
        std::unique_ptr<nlmatrix::MatrixRing> asioInputRing;
        std::unique_ptr<WasapiLoopbackCapture> loopback;
        std::unique_ptr<ProcessLoopbackCapture> processCapture;
        std::thread processWatch;
        std::atomic<bool> processWatchStop { false };
        std::atomic<bool> online { true };
        std::atomic<bool> enabled { true };
        std::atomic<bool> muted { false };
        float smoothedGain = 1.0f;
        std::atomic<float> targetGainDb { 0.0f };
        std::unique_ptr<NlDriverClient> driver;
        std::thread driverThread;
        std::atomic<bool> driverStop { false };
        std::unique_ptr<juce::AudioIODevice> captureDevice;
        std::unique_ptr<CaptureCallback> captureCallback;
        int busIndex = -1;
        int channels = 2;
        std::vector<int> asioCompact;
        std::vector<std::vector<float>> buf;
        std::vector<double> testPhases, testFrequencies;
        std::atomic<double> sampleRate { 0.0 };
        nlmatrix::MatrixRateConverter converter; // audio callback only
        std::atomic<float> peak { 0.0f };
        std::atomic<float> peakL { 0.0f };
        std::atomic<float> peakR { 0.0f };
        std::array<std::atomic<float>, 32> channelPeak {};
        std::atomic<bool> clipped { false };
    };

    struct ResolvedRoute
    {
        int sourceIndex = -1;
        float gainL = 1.0f;
        float gainR = 1.0f;
        std::vector<int> channelMap;
    };

    struct BusRuntime
    {
        BusDef def;
        std::vector<ResolvedRoute> routes;
        int channels = 2;
        std::vector<std::vector<float>> mix;
        std::atomic<bool> muted { false };
        float smoothedGain = 1.0f;
        std::atomic<float> targetGainDb { 0.0f };
        std::atomic<float> peak { 0.0f };
        std::atomic<float> peakL { 0.0f };
        std::atomic<float> peakR { 0.0f };
        std::array<std::atomic<float>, 32> channelPeak {};
        std::atomic<bool> clipped { false };
    };

    struct DestinationRuntime
    {
        DestinationDef def;
        std::unique_ptr<nlmatrix::MatrixRing> ring;
        std::unique_ptr<nlmatrix::MatrixRing> asioOutputRing;
        std::unique_ptr<nlmatrix::MatrixRateConverter> asioOutputConverter;
        std::atomic<double> matrixRate { 0.0 };
        std::unique_ptr<juce::AudioIODevice> renderDevice;
        std::unique_ptr<RenderCallback> renderCallback;
        std::thread processWatch;
        std::atomic<bool> processWatchStop { false };
        std::atomic<bool> online { true };
        std::atomic<bool> enabled { true };
        std::unique_ptr<NlDriverClient> driver;
        std::thread driverThread;
        std::atomic<bool> driverStop { false };
        std::atomic<uint64_t> driverUnderruns { 0 };
        std::atomic<int64_t> lastDriverWriteMs { 0 };
        int channels = 2;
        std::vector<int> asioCompact;
        std::atomic<double> sampleRate { 0.0 };
        std::vector<ResolvedRoute> routes;
        std::vector<std::vector<float>> mix;
        std::atomic<bool> muted { false };
        float smoothedGain = 1.0f;
        std::atomic<float> targetGainDb { 0.0f };
        std::atomic<float> peak { 0.0f };
        std::atomic<float> peakL { 0.0f };
        std::atomic<float> peakR { 0.0f };
        std::array<std::atomic<float>, 32> channelPeak {};
        std::atomic<bool> clipped { false };
    };

    // One additional full-duplex ASIO device.  Its callback only moves audio
    // between its hardware buffers and SPSC rings; the graph master owns the
    // matrix and clock.  This is the first same-process multi-ASIO path.
    class ExtraAsioDevice
    {
    public:
        bool open (const juce::String& name, bool needInput, bool needOutput,
                   double requestedRate, juce::String& error)
        {
            auto* type = juce::AudioIODeviceType::createAudioIODeviceType_ASIO();
            if (type == nullptr)
            {
                error = "ASIO unavailable";
                return false;
            }
            type->scanForDevices();
            device.reset (type->createDevice (name, name));
            if (device == nullptr)
            {
                error = "cannot create " + name;
                return false;
            }
            const auto ins = device->getInputChannelNames();
            const auto outs = device->getOutputChannelNames();
            juce::BigInteger inMask, outMask;
            if (needInput) inMask.setRange (0, ins.size(), true);
            if (needOutput) outMask.setRange (0, outs.size(), true);
            auto err = device->open (inMask, outMask, requestedRate, 256);
            if (err.isNotEmpty() && needInput && ! needOutput)
            {
                outMask.setRange (0, outs.size(), true);
                err = device->open (inMask, outMask, requestedRate, 256);
            }
            if (err.isNotEmpty())
            {
                error = name + ": " + err;
                return false;
            }
            const auto activeIn = device->getActiveInputChannels();
            const auto activeOut = device->getActiveOutputChannels();
            const int inputCount = juce::jmax (1, ins.size());
            const int outputCount = juce::jmax (1, outs.size());
            inMap.assign ((size_t) inputCount, -1);
            outMap.assign ((size_t) outputCount, -1);
            int compact = 0;
            for (int i = 0; i < inputCount; ++i)
                if (activeIn[i]) inMap[(size_t) i] = compact++;
            compact = 0;
            for (int i = 0; i < outputCount; ++i)
                if (activeOut[i]) outMap[(size_t) i] = compact++;
            rate = device->getCurrentSampleRate();
            openedInputs = (int) ins.size();
            openedOutputs = (int) outs.size();
            return true;
        }

        void attach (std::vector<SourceRuntime*> newSources,
                     std::vector<DestinationRuntime*> newDestinations,
                     double graphRate)
        {
            attachedSources = std::move (newSources);
            attachedDestinations = std::move (newDestinations);
            for (auto* s : attachedSources)
                if (s != nullptr)
                    s->sampleRate.store (rate, std::memory_order_release);
            for (auto* d : attachedDestinations)
                if (d != nullptr)
                {
                    d->matrixRate.store (graphRate, std::memory_order_release);
                    if (d->asioOutputConverter == nullptr)
                        d->asioOutputConverter = std::make_unique<nlmatrix::MatrixRateConverter> (d->channels);
                    else
                        d->asioOutputConverter->reset (d->channels);
                }
            callback = std::make_unique<Callback> (device.get(), inMap, outMap, rate);
            callback->sources = attachedSources;
            callback->destinations = attachedDestinations;
        }

        void start()
        {
            if (device != nullptr && callback != nullptr)
                device->start (callback.get());
        }

        bool isPlaying() const { return device != nullptr && device->isPlaying(); }

        void stop()
        {
            if (device != nullptr)
            {
                device->stop();
                device->close();
            }
            callback.reset();
            device.reset();
        }

        double getRate() const { return rate; }
        int getOpenedInputs() const { return openedInputs; }
        int getOpenedOutputs() const { return openedOutputs; }

    private:
        class Callback final : public juce::AudioIODeviceCallback
        {
        public:
            Callback (juce::AudioIODevice* d, std::vector<int> inMapIn, std::vector<int> outMapIn,
                      double deviceRateIn)
                : device (d), inMap (std::move (inMapIn)), outMap (std::move (outMapIn)),
                  deviceRate (deviceRateIn) {}

            void audioDeviceIOCallbackWithContext (const float* const* input, int numInputs,
                                                   float* const* output, int numOutputs, int numSamples,
                                                   const juce::AudioIODeviceCallbackContext&) override
            {
                juce::ignoreUnused (device);
                for (int ch = 0; ch < numOutputs; ++ch)
                    if (output[ch] != nullptr)
                        juce::FloatVectorOperations::clear (output[ch], numSamples);

                for (auto* source : sources)
                {
                    if (source == nullptr || source->asioInputRing == nullptr)
                        continue;
                    const int channels = juce::jmin (32, (int) source->def.channels.size());
                    for (int ch = 0; ch < channels; ++ch)
                    {
                        const int physical = source->def.channels[(size_t) ch];
                        const int compact = physical >= 0 && physical < (int) inMap.size()
                                                ? inMap[(size_t) physical] : -1;
                        inputPtrs[(size_t) ch] = compact >= 0 && compact < numInputs
                                                     ? input[compact] : nullptr;
                    }
                    source->asioInputRing->write (inputPtrs.data(), channels, numSamples);
                }

                for (auto* destination : destinations)
                {
                    if (destination == nullptr || destination->asioOutputRing == nullptr)
                        continue;
                    const int channels = juce::jmin (32, destination->channels);
                    for (int ch = 0; ch < channels; ++ch)
                    {
                        const int physical = ch < (int) destination->def.channels.size()
                                                 ? destination->def.channels[(size_t) ch] : -1;
                        const int compact = physical >= 0 && physical < (int) outMap.size()
                                                ? outMap[(size_t) physical] : -1;
                        outputPtrs[(size_t) ch] = compact >= 0 && compact < numOutputs
                                                      ? output[compact] : nullptr;
                    }
                    const double graphRate = destination->matrixRate.load (std::memory_order_acquire);
                    if (destination->asioOutputConverter != nullptr
                        && graphRate > 0.0 && std::abs (graphRate - deviceRate) > 0.5)
                    {
                        destination->asioOutputConverter->read (
                            *destination->asioOutputRing, outputPtrs.data(), channels, numSamples,
                            graphRate, deviceRate,
                            juce::jmax (256, (int) std::lround (graphRate * 0.015)));
                    }
                    else
                        destination->asioOutputRing->read (outputPtrs.data(), channels, numSamples);
                }
            }

            void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
            void audioDeviceStopped() override {}
            void audioDeviceError (const juce::String&) override {}

            juce::AudioIODevice* device = nullptr;
            std::vector<int> inMap, outMap;
            std::vector<SourceRuntime*> sources;
            std::vector<DestinationRuntime*> destinations;
            std::array<const float*, 32> inputPtrs {};
            std::array<float*, 32> outputPtrs {};
            double deviceRate = 48000.0;
        };

        std::unique_ptr<juce::AudioIODevice> device;
        std::unique_ptr<Callback> callback;
        std::vector<int> inMap, outMap;
        std::vector<SourceRuntime*> attachedSources;
        std::vector<DestinationRuntime*> attachedDestinations;
        double rate = 48000.0;
        int openedInputs = 0;
        int openedOutputs = 0;
    };
    // Immutable routing table. The audio thread only reads a published
    // snapshot; the control thread builds a replacement and swaps the pointer.
    // Old snapshots are retained by runRouter until shutdown, so no reclamation
    // can race the audio callback.
    struct GraphSnapshot
    {
        std::vector<std::vector<ResolvedRoute>> busRoutes;
        std::vector<std::vector<ResolvedRoute>> destinationRoutes;
        std::vector<int> busOrder;
        std::vector<bool> sourceEnabled, destinationEnabled, busEnabled;
        std::vector<bool> sourceSolo, busSolo, destinationSolo, destinationMonitor;
        // destination index -> source indices excluded for mix-minus
        std::vector<std::vector<bool>> destinationExcluded;
        // When the graph has no enabled route there is nothing to process.
        // Keep the device clock and telemetry alive, but skip all source reads
        // and channel mixing until a route becomes active again.
        bool active = true;
    };

    class RouterCallback final : public juce::AudioIODeviceCallback
    {
    public:
        RouterCallback (std::vector<SourceRuntime*> s, std::vector<DestinationRuntime*> d,
                        std::vector<BusRuntime*> b, std::vector<int> order,
                        std::atomic<const GraphSnapshot*>& graphRef)
            : sources (std::move (s)), destinations (std::move (d)),
              buses (std::move (b)), busOrder (std::move (order)), graph (graphRef) {}

        void audioDeviceAboutToStart (juce::AudioIODevice* device) override
        {
            const int block = device != nullptr ? device->getCurrentBufferSizeSamples() : 256;
            maxBlock = juce::jmax (2048, block);
            callbackRate = device != nullptr ? device->getCurrentSampleRate() : 48000.0;
            if (! (callbackRate > 0.0))
                callbackRate = 48000.0;
            for (auto* s : sources)
            {
                s->channels = juce::jlimit (1, 32, s->channels);
                s->buf.assign ((size_t) s->channels, std::vector<float> ((size_t) maxBlock, 0.0f));
                s->converter.reset (s->channels);
                for (auto& peak : s->channelPeak) peak.store (0.0f);
            }
            for (auto* d : destinations)
            {
                d->channels = juce::jlimit (1, 32, d->channels);
                d->mix.assign ((size_t) d->channels, std::vector<float> ((size_t) maxBlock, 0.0f));
                for (auto& peak : d->channelPeak) peak.store (0.0f);
            }
            for (auto* b : buses)
            {
                b->channels = juce::jlimit (1, 32, b->channels);
                b->mix.assign ((size_t) b->channels, std::vector<float> ((size_t) maxBlock, 0.0f));
                for (auto& peak : b->channelPeak) peak.store (0.0f);
            }
        }

        void audioDeviceStopped() override {}
        void audioDeviceError (const juce::String&) override { error.store (true); }

        void audioDeviceIOCallbackWithContext (const float* const* input, int numInputs,
                                               float* const* output, int numOutputs, int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            for (int ch = 0; ch < numOutputs; ++ch)
                if (output[ch] != nullptr)
                    juce::FloatVectorOperations::clear (output[ch], numSamples);

            lastCallbackMs.store (juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
            framesProcessed.fetch_add ((uint64_t) juce::jmax (0, numSamples), std::memory_order_relaxed);
            const int n = juce::jmin (numSamples, maxBlock);
            if (n <= 0)
                return;
            const auto* snapshot = graph.load (std::memory_order_acquire);
            if (snapshot == nullptr)
                return;
            if (! snapshot->active)
            {
                callbacks.fetch_add (1);
                return;
            }

            auto sourceChannels = [&] (int sourceIndex) -> const std::vector<std::vector<float>>*
            {
                if (sourceIndex < 0 || sourceIndex >= (int) sources.size())
                    return nullptr;
                const auto* s = sources[(size_t) sourceIndex];
                if (s->def.type == "bus")
                {
                    if (s->busIndex < 0 || s->busIndex >= (int) buses.size())
                        return nullptr;
                    return &buses[(size_t) s->busIndex]->mix;
                }
                return &s->buf;
            };

            auto rampGain = [n] (std::vector<std::vector<float>>& buffers, float& smoothed,
                            float gainDb)
            {
                const float target = std::pow (10.0f, gainDb / 20.0f);
                if (std::abs (target - smoothed) < 1.0e-6f)
                {
                    if (std::abs (target - 1.0f) > 1.0e-6f)
                        for (auto& channel : buffers)
                            for (int i = 0; i < n; ++i) channel[(size_t) i] *= target;
                    smoothed = target;
                    return;
                }
                const float step = (target - smoothed) / (float) juce::jmax (1, n);
                for (auto& channel : buffers)
                {
                    float g = smoothed;
                    for (int i = 0; i < n; ++i)
                    {
                        g += step;
                        channel[(size_t) i] *= g;
                    }
                }
                smoothed = target;
            };

            for (size_t si = 0; si < sources.size(); ++si)
            {
                auto* s = sources[si];
                if (s->def.type == "bus")
                    continue;
                for (auto& channel : s->buf)
                    std::fill (channel.begin(), channel.begin() + n, 0.0f);

                if (s->def.type == "test")
                {
                    for (int ch = 0; ch < s->channels; ++ch)
                    {
                        const double step = 2.0 * juce::MathConstants<double>::pi
                                            * s->testFrequencies[(size_t) ch] / callbackRate;
                        auto& phase = s->testPhases[(size_t) ch];
                        auto& destination = s->buf[(size_t) ch];
                        for (int i = 0; i < n; ++i)
                        {
                            destination[(size_t) i] = (float) s->def.testLevel * (float) std::sin (phase);
                            phase += step;
                        }
                    }
                }
                else if (s->asioInputRing != nullptr)
                {
                    const double sourceRate = s->sampleRate.load (std::memory_order_acquire);
                    const double effectiveRate = sourceRate > 0.0 ? sourceRate : callbackRate;
                    std::vector<float*> pointers ((size_t) s->channels, nullptr);
                    for (int ch = 0; ch < s->channels; ++ch)
                        pointers[(size_t) ch] = s->buf[(size_t) ch].data();
                    if (std::abs (effectiveRate - callbackRate) <= 0.5)
                        s->asioInputRing->read (pointers.data(), s->channels, n);
                    else
                        s->converter.read (*s->asioInputRing, pointers.data(), s->channels, n,
                                           effectiveRate, callbackRate,
                                           juce::jmax (256, (int) std::lround (effectiveRate * 0.015)));
                }
                else if (s->def.type == "asio-in")
                {
                    for (int ch = 0; ch < s->channels; ++ch)
                    {
                        const int physical = ch < (int) s->asioCompact.size()
                                                 ? s->asioCompact[(size_t) ch] : -1;
                        if (physical >= 0 && physical < numInputs && input[physical] != nullptr)
                            std::copy (input[physical], input[physical] + n,
                                       s->buf[(size_t) ch].begin());
                    }
                }
                else
                {
                    const double sourceRate = s->sampleRate.load (std::memory_order_acquire);
                    const double effectiveRate = sourceRate > 0.0 ? sourceRate : callbackRate;
                    std::vector<float*> pointers ((size_t) s->channels, nullptr);
                    for (int ch = 0; ch < s->channels; ++ch)
                        pointers[(size_t) ch] = s->buf[(size_t) ch].data();
                    if (std::abs (effectiveRate - callbackRate) <= 0.5)
                        s->ring->read (pointers.data(), s->channels, n);
                    else
                        s->converter.read (*s->ring, pointers.data(), s->channels, n,
                                           effectiveRate, callbackRate,
                                           juce::jmax (256, (int) std::lround (effectiveRate * 0.015)));
                }

                const bool sourceEnabled = si < snapshot->sourceEnabled.size()
                                               ? snapshot->sourceEnabled[si] : s->def.enabled;
                if (! sourceEnabled || s->muted.load())
                    for (auto& channel : s->buf)
                        std::fill (channel.begin(), channel.begin() + n, 0.0f);
                else
                    rampGain (s->buf, s->smoothedGain, s->targetGainDb.load());
                    {
                        const float p = juce::jlimit (-1.0f, 1.0f, s->def.pan);
                        const float lg = p > 0.0f ? 1.0f - p : 1.0f;
                        const float rg = p < 0.0f ? 1.0f + p : 1.0f;
                        const float polarity = s->def.polarity ? -1.0f : 1.0f;
                        for (int ch = 0; ch < s->channels; ++ch)
                        {
                            const float factor = polarity * (s->channels > 1
                                                              ? (ch == 0 ? lg : ch == 1 ? rg : 1.0f)
                                                              : 1.0f);
                            if (std::abs (factor - 1.0f) > 1.0e-6f)
                                for (int i = 0; i < n; ++i)
                                    s->buf[(size_t) ch][(size_t) i] *= factor;
                        }
                    }

                float pl = 0.0f, pr = 0.0f;
                bool clip = false;
                for (int ch = 0; ch < s->channels; ++ch)
                {
                    const auto& channel = s->buf[(size_t) ch];
                    float peak = 0.0f;
                    for (int i = 0; i < n; ++i)
                    {
                        peak = juce::jmax (peak, std::abs (channel[(size_t) i]));
                        clip = clip || std::abs (channel[(size_t) i]) > 1.0f;
                    }
                    if (ch == 0) pl = peak;
                    if (ch == 1) pr = peak;
                }
                for (int ch = 0; ch < s->channels; ++ch)
                {
                    float channelPeak = 0.0f;
                    for (int i = 0; i < n; ++i)
                        channelPeak = juce::jmax (channelPeak, std::abs (s->buf[(size_t) ch][(size_t) i]));
                    s->channelPeak[(size_t) ch].store (juce::jmax (s->channelPeak[(size_t) ch].load(), channelPeak));
                }
                if (pl > s->peakL.load()) s->peakL.store (pl);
                if (pr > s->peakR.load()) s->peakR.store (pr);
                if (clip) s->clipped.store (true);
                s->peak.store (juce::jmax (pl, pr));
            }

            const auto& order = snapshot->busOrder.empty() ? busOrder : snapshot->busOrder;
            for (int bi : order)
            {
                auto* b = buses[(size_t) bi];
                for (auto& channel : b->mix)
                    std::fill (channel.begin(), channel.begin() + n, 0.0f);
                const auto& routes = ((size_t) bi < snapshot->busRoutes.size())
                                         ? snapshot->busRoutes[(size_t) bi] : b->routes;
                for (const auto& r : routes)
                {
                    const auto* source = sourceChannels (r.sourceIndex);
                    if (source == nullptr)
                        continue;
                    for (int destCh = 0; destCh < b->channels; ++destCh)
                    {
                        const int srcCh = destCh < (int) r.channelMap.size()
                                              ? r.channelMap[(size_t) destCh] : destCh;
                        if (srcCh < 0 || srcCh >= (int) source->size())
                            continue;
                        const auto& src = (*source)[(size_t) srcCh];
                        auto& dst = b->mix[(size_t) destCh];
                        const float gain = destCh == 1 ? r.gainR : r.gainL;
                        for (int i = 0; i < n; ++i)
                            dst[(size_t) i] += src[(size_t) i] * gain;
                    }
                }
                const bool busEnabled = (size_t) bi < snapshot->busEnabled.size()
                                            ? snapshot->busEnabled[(size_t) bi] : b->def.enabled;
                if (! busEnabled || b->muted.load())
                    for (auto& channel : b->mix)
                        std::fill (channel.begin(), channel.begin() + n, 0.0f);
                else
                    rampGain (b->mix, b->smoothedGain, b->targetGainDb.load());

                float pl = 0.0f, pr = 0.0f;
                bool clip = false;
                for (int ch = 0; ch < b->channels; ++ch)
                {
                    auto& channel = b->mix[(size_t) ch];
                    for (int i = 0; i < n; ++i)
                    {
                        clip = clip || std::abs (channel[(size_t) i]) > 1.0f;
                        channel[(size_t) i] = juce::jlimit (-1.0f, 1.0f, channel[(size_t) i]);
                    }
                    float peak = 0.0f;
                    for (int i = 0; i < n; ++i)
                        peak = juce::jmax (peak, std::abs (channel[(size_t) i]));
                    if (ch == 0) pl = peak;
                    if (ch == 1) pr = peak;
                }
                for (int ch = 0; ch < b->channels; ++ch)
                {
                    float channelPeak = 0.0f;
                    for (int i = 0; i < n; ++i)
                        channelPeak = juce::jmax (channelPeak, std::abs (b->mix[(size_t) ch][(size_t) i]));
                    b->channelPeak[(size_t) ch].store (juce::jmax (b->channelPeak[(size_t) ch].load(), channelPeak));
                }
                if (pl > b->peakL.load()) b->peakL.store (pl);
                if (pr > b->peakR.load()) b->peakR.store (pr);
                if (clip) b->clipped.store (true);
                b->peak.store (juce::jmax (pl, pr));
            }

            for (size_t di = 0; di < destinations.size(); ++di)
            {
                auto* d = destinations[di];
                for (auto& channel : d->mix)
                    std::fill (channel.begin(), channel.begin() + n, 0.0f);
                const auto& routes = di < snapshot->destinationRoutes.size()
                                         ? snapshot->destinationRoutes[di] : d->routes;
                const bool monitorDestination = di < snapshot->destinationMonitor.size()
                                                    ? snapshot->destinationMonitor[di] : d->def.monitor;
                const bool anySolo = std::any_of (snapshot->sourceSolo.begin(), snapshot->sourceSolo.end(),
                                                      [] (bool v) { return v; })
                                     || std::any_of (snapshot->busSolo.begin(), snapshot->busSolo.end(),
                                                      [] (bool v) { return v; })
                                     || std::any_of (snapshot->destinationSolo.begin(), snapshot->destinationSolo.end(),
                                                      [] (bool v) { return v; });
                const bool destinationSolo = di < snapshot->destinationSolo.size()
                                                 ? snapshot->destinationSolo[di] : false;
                const bool anyDestinationSolo = std::any_of (snapshot->destinationSolo.begin(),
                                                             snapshot->destinationSolo.end(),
                                                             [] (bool v) { return v; });
                for (const auto& r : routes)
                {
                    if (di < snapshot->destinationExcluded.size()
                        && r.sourceIndex >= 0
                        && r.sourceIndex < (int) snapshot->destinationExcluded[di].size()
                        && snapshot->destinationExcluded[di][(size_t) r.sourceIndex])
                        continue;
                    const auto* source = sourceChannels (r.sourceIndex);
                    if (source == nullptr)
                        continue;
                    if (monitorDestination && anySolo)
                    {
                        if (anyDestinationSolo && ! destinationSolo)
                            continue;
                        bool sourceSolo = r.sourceIndex >= 0
                                          && r.sourceIndex < (int) snapshot->sourceSolo.size()
                                          && snapshot->sourceSolo[(size_t) r.sourceIndex];
                        if (! sourceSolo && r.sourceIndex >= 0 && r.sourceIndex < (int) sources.size())
                        {
                            const auto* sourceRuntime = sources[(size_t) r.sourceIndex];
                            if (sourceRuntime != nullptr && sourceRuntime->busIndex >= 0
                                && sourceRuntime->busIndex < (int) snapshot->busSolo.size())
                                sourceSolo = snapshot->busSolo[(size_t) sourceRuntime->busIndex];
                        }
                        if (! sourceSolo)
                            continue;
                    }
                    for (int destCh = 0; destCh < d->channels; ++destCh)
                    {
                        const int srcCh = destCh < (int) r.channelMap.size()
                                              ? r.channelMap[(size_t) destCh] : destCh;
                        if (srcCh < 0 || srcCh >= (int) source->size())
                            continue;
                        const auto& src = (*source)[(size_t) srcCh];
                        auto& dst = d->mix[(size_t) destCh];
                        const float gain = destCh == 1 ? r.gainR : r.gainL;
                        for (int i = 0; i < n; ++i)
                            dst[(size_t) i] += src[(size_t) i] * gain;
                    }
                }

                const bool destinationEnabled = di < snapshot->destinationEnabled.size()
                                                    ? snapshot->destinationEnabled[di] : d->def.enabled;
                if (! destinationEnabled || d->muted.load())
                    for (auto& channel : d->mix)
                        std::fill (channel.begin(), channel.begin() + n, 0.0f);
                else
                    rampGain (d->mix, d->smoothedGain, d->targetGainDb.load());

                float pl = 0.0f, pr = 0.0f;
                bool clip = false;
                for (int ch = 0; ch < d->channels; ++ch)
                {
                    auto& channel = d->mix[(size_t) ch];
                    for (int i = 0; i < n; ++i)
                    {
                        clip = clip || std::abs (channel[(size_t) i]) > 1.0f;
                        channel[(size_t) i] = juce::jlimit (-1.0f, 1.0f, channel[(size_t) i]);
                    }
                    float peak = 0.0f;
                    for (int i = 0; i < n; ++i)
                        peak = juce::jmax (peak, std::abs (channel[(size_t) i]));
                    if (ch == 0) pl = peak;
                    if (ch == 1) pr = peak;
                }
                for (int ch = 0; ch < d->channels; ++ch)
                {
                    float channelPeak = 0.0f;
                    for (int i = 0; i < n; ++i)
                        channelPeak = juce::jmax (channelPeak, std::abs (d->mix[(size_t) ch][(size_t) i]));
                    d->channelPeak[(size_t) ch].store (juce::jmax (d->channelPeak[(size_t) ch].load(), channelPeak));
                }
                if (pl > d->peakL.load()) d->peakL.store (pl);
                if (pr > d->peakR.load()) d->peakR.store (pr);
                if (clip) d->clipped.store (true);
                d->peak.store (juce::jmax (pl, pr));

                if (d->asioOutputRing != nullptr)
                {
                    std::vector<const float*> pointers ((size_t) d->channels, nullptr);
                    for (int ch = 0; ch < d->channels; ++ch)
                        pointers[(size_t) ch] = d->mix[(size_t) ch].data();
                    d->asioOutputRing->write (pointers.data(), d->channels, n);
                }
                else if (d->def.type == "asio-out")
                {
                    for (int ch = 0; ch < d->channels; ++ch)
                    {
                        const int physical = ch < (int) d->asioCompact.size()
                                                 ? d->asioCompact[(size_t) ch] : -1;
                        if (physical >= 0 && physical < numOutputs && output[physical] != nullptr)
                            std::copy (d->mix[(size_t) ch].begin(),
                                       d->mix[(size_t) ch].begin() + n, output[physical]);
                    }
                }
                else
                {
                    std::vector<const float*> pointers ((size_t) d->channels, nullptr);
                    for (int ch = 0; ch < d->channels; ++ch)
                        pointers[(size_t) ch] = d->mix[(size_t) ch].data();
                    d->ring->write (pointers.data(), d->channels, n);
                }
            }

            callbacks.fetch_add (1);
        }

        std::atomic<bool> error { false };
        std::atomic<uint64_t> callbacks { 0 };
        std::atomic<double> lastCallbackMs { 0.0 };
        std::atomic<uint64_t> framesProcessed { 0 };

    private:
        std::vector<SourceRuntime*> sources;
        std::vector<DestinationRuntime*> destinations;
        std::vector<BusRuntime*> buses;
        std::vector<int> busOrder;
        std::atomic<const GraphSnapshot*>& graph;
        int maxBlock = 2048;
        double callbackRate = 48000.0;
    };
    //==============================================================================
    static bool buildRoutingSnapshot (
        const Config& cfg,
        const std::vector<std::unique_ptr<SourceRuntime>>& sources,
        const std::map<juce::String, int>& sourceIndex,
        const std::map<juce::String, int>& destinationIndex,
        const std::map<juce::String, int>& busIndex,
        GraphSnapshot& out, juce::String& error)
    {
        out.busRoutes.assign (busIndex.size(), {});
        out.destinationRoutes.assign (destinationIndex.size(), {});
        out.destinationExcluded.assign (destinationIndex.size(),
                                        std::vector<bool> (sourceIndex.size(), false));
        out.sourceSolo.assign (sourceIndex.size(), false);
        out.busSolo.assign (busIndex.size(), false);
        out.destinationSolo.assign (destinationIndex.size(), false);
        out.destinationMonitor.assign (destinationIndex.size(), false);
        for (const auto& s : cfg.sources)
            if (const auto it = sourceIndex.find (s.id); it != sourceIndex.end())
                out.sourceSolo[(size_t) it->second] = s.solo;
        for (const auto& b : cfg.buses)
            if (const auto it = busIndex.find (b.id); it != busIndex.end())
                out.busSolo[(size_t) it->second] = b.solo;
        for (const auto& d : cfg.destinations)
            if (const auto it = destinationIndex.find (d.id); it != destinationIndex.end())
            {
                out.destinationSolo[(size_t) it->second] = d.solo;
                out.destinationMonitor[(size_t) it->second] = d.monitor;
            }
        for (const auto& d : cfg.destinations)
        {
            const auto di = destinationIndex.find (d.id);
            if (di == destinationIndex.end())
                continue;
            for (const auto& excluded : d.mixMinusSources)
            {
                const auto si = sourceIndex.find (excluded);
                if (si != sourceIndex.end())
                    out.destinationExcluded[(size_t) di->second][(size_t) si->second] = true;
            }
        }
        out.busOrder.clear();
        out.sourceEnabled.clear();
        out.destinationEnabled.clear();
        out.busEnabled.clear();
        for (const auto& s : cfg.sources) out.sourceEnabled.push_back (s.enabled);
        for (const auto& d : cfg.destinations) out.destinationEnabled.push_back (d.enabled);
        for (const auto& b : cfg.buses) out.busEnabled.push_back (b.enabled);
        out.active = false;

        for (const auto& route : cfg.routes)
        {
            if (! route.enabled)
                continue;
            const auto si = sourceIndex.find (route.sourceId);
            if (si == sourceIndex.end())
            {
                error = "route source not found: " + route.sourceId;
                return false;
            }
            ResolvedRoute resolved;
            resolved.sourceIndex = si->second;
            resolved.gainL = route.gainL;
            resolved.gainR = route.gainR;
            resolved.channelMap = route.channelMap;

            const auto di = destinationIndex.find (route.destinationId);
            const auto bi = busIndex.find (route.destinationId);
            if (di != destinationIndex.end() || bi != busIndex.end())
                out.active = true;
            if (di != destinationIndex.end())
                out.destinationRoutes[(size_t) di->second].push_back (resolved);
            else if (bi != busIndex.end())
                out.busRoutes[(size_t) bi->second].push_back (resolved);
            else
            {
                error = "route destination not found: " + route.destinationId;
                return false;
            }
        }

        std::vector<std::vector<int>> edges (out.busRoutes.size());
        std::vector<int> indegree (out.busRoutes.size(), 0);
        for (int bi = 0; bi < (int) out.busRoutes.size(); ++bi)
        {
            for (const auto& r : out.busRoutes[(size_t) bi])
            {
                if (r.sourceIndex < 0 || r.sourceIndex >= (int) sources.size())
                    continue;
                const auto* s = sources[(size_t) r.sourceIndex].get();
                if (s->def.type == "bus" && s->busIndex >= 0)
                {
                    edges[(size_t) s->busIndex].push_back (bi);
                    indegree[(size_t) bi]++;
                }
            }
        }

        std::vector<int> ready;
        for (int i = 0; i < (int) indegree.size(); ++i)
            if (indegree[(size_t) i] == 0)
                ready.push_back (i);
        while (! ready.empty())
        {
            const int bi = ready.back();
            ready.pop_back();
            out.busOrder.push_back (bi);
            for (int next : edges[(size_t) bi])
                if (--indegree[(size_t) next] == 0)
                    ready.push_back (next);
        }
        if (out.busOrder.size() != out.busRoutes.size())
        {
            error = "bus graph contains a cycle";
            return false;
        }
        return true;
    }

    static bool sameTopology (const Config& a, const Config& b)
    {
        if (a.asio != b.asio || a.sources.size() != b.sources.size()
            || a.destinations.size() != b.destinations.size()
            || a.buses.size() != b.buses.size())
            return false;
        for (size_t i = 0; i < a.sources.size(); ++i)
        {
            const auto& x = a.sources[i];
            const auto& y = b.sources[i];
            if (x.id != y.id || x.type != y.type || x.endpoint != y.endpoint
                || x.process != y.process || x.busId != y.busId)
                return false;
        }
        for (size_t i = 0; i < a.destinations.size(); ++i)
        {
            const auto& x = a.destinations[i];
            const auto& y = b.destinations[i];
            if (x.id != y.id || x.type != y.type || x.endpoint != y.endpoint)
                return false;
        }
        for (size_t i = 0; i < a.buses.size(); ++i)
            if (a.buses[i].id != b.buses[i].id)
                return false;
        return true;
    }

    static double idleExitSeconds = 0.0;
    static juce::String inspectAsioName;
    static juce::String telemetryPath;
    static juce::String diagnosticsPath;

    int runRouter (const juce::File& configFile, double seconds)
    {
        Config config;
        juce::String error;
        if (! loadConfig (configFile, config, error))
        {
            std::cerr << "Config error: " << error << "\n";
            return 1;
        }

        std::cout << "Config: " << configFile.getFullPathName() << "\n";
        std::cout << "ASIO: " << config.asio << "\n";
        std::cout << "Sources: " << config.sources.size()
                  << ", destinations: " << config.destinations.size()
                  << ", routes: " << config.routes.size() << "\n";

        double requestedRate = 48000.0;
        int requestedBuffer = 256;
        for (const auto& deviceDef : config.devices)
            if (deviceDef.backend == "asio"
                && (deviceDef.clockMaster || deviceDef.driver == config.asio))
            {
                if (deviceDef.sampleRate > 0.0) requestedRate = deviceDef.sampleRate;
                if (deviceDef.bufferSize > 0) requestedBuffer = deviceDef.bufferSize;
                break;
            }

        auto at = asioType();
        if (at == nullptr)
        {
            std::cerr << "ASIO unavailable.\n";
            return 2;
        }
        at->scanForDevices();
        const auto asioNames = at->getDeviceNames (false);
        const auto asioName = chooseName (asioNames, config.asio, "Universal Audio");
        if (asioName.isEmpty())
        {
            std::cerr << "ASIO device not found: " << config.asio << "\n";
            return 3;
        }

        std::unique_ptr<juce::AudioIODevice> asioDevice (at->createDevice (asioName, asioName));
        if (asioDevice == nullptr)
        {
            std::cerr << "Could not create ASIO device.\n";
            return 4;
        }

        const auto asioIns = asioDevice->getInputChannelNames();
        const auto asioOuts = asioDevice->getOutputChannelNames();
        const bool needOutputs = std::any_of (config.destinations.begin(), config.destinations.end(),
                                              [] (const DestinationDef& d) { return d.type == "asio-out"; });

        juce::BigInteger inMask, outMask;
        inMask.setRange (0, asioIns.size(), true);
        if (needOutputs)
            outMask.setRange (0, asioOuts.size(), true);

        auto asioError = asioDevice->open (inMask, outMask, requestedRate, requestedBuffer);
        if (asioError.isNotEmpty() && ! needOutputs)
        {
            outMask.setRange (0, asioOuts.size(), true);
            asioError = asioDevice->open (inMask, outMask, requestedRate, requestedBuffer);
        }
        if (asioError.isNotEmpty())
        {
            std::cerr << "Could not open ASIO device: " << asioError << "\n";
            return 5;
        }

        const double asioRate = asioDevice->getCurrentSampleRate();
        std::cout << "ASIO rate: " << asioRate << " Hz, buffer "
                  << asioDevice->getCurrentBufferSizeSamples() << "\n";

        const auto activeIn = asioDevice->getActiveInputChannels();
        const auto activeOut = asioDevice->getActiveOutputChannels();
        std::vector<int> inMap ((size_t) asioIns.size(), -1), outMap ((size_t) asioOuts.size(), -1);
        int compact = 0;
        for (int i = 0; i < asioIns.size(); ++i)
            if (activeIn[i]) inMap[(size_t) i] = compact++;
        compact = 0;
        for (int i = 0; i < asioOuts.size(); ++i)
            if (activeOut[i]) outMap[(size_t) i] = compact++;

        auto wt = wasapiType();
        if (wt == nullptr)
        {
            std::cerr << "WASAPI unavailable.\n";
            return 6;
        }
        wt->scanForDevices();
        const auto winIns = wt->getDeviceNames (true);
        const auto winOuts = wt->getDeviceNames (false);

        // Open every additional ASIO driver used by the project.  The master
        // driver below still owns the graph clock; extra devices exchange
        // samples through per-source/per-destination rings.
        std::map<juce::String, std::unique_ptr<ExtraAsioDevice>> extraAsioDevices;
        std::set<juce::String> extraAsioIds;
        for (const auto& deviceDef : config.devices)
        {
            if (deviceDef.backend != "asio" || deviceDef.id.isEmpty())
                continue;
            const auto driver = deviceDef.driver.isNotEmpty() ? deviceDef.driver : deviceDef.displayName;
            if (driver.isEmpty() || driver == config.asio)
                continue;
            const bool needInputs = std::any_of (config.sources.begin(), config.sources.end(),
                                                 [&] (const SourceDef& s) { return s.deviceId == deviceDef.id; });
            const bool needOutputs = std::any_of (config.destinations.begin(), config.destinations.end(),
                                                  [&] (const DestinationDef& d) { return d.deviceId == deviceDef.id; });
            if (! needInputs && ! needOutputs)
                continue;
            auto extra = std::make_unique<ExtraAsioDevice>();
            juce::String openError;
            if (! extra->open (driver, needInputs, needOutputs, deviceDef.sampleRate, openError))
            {
                std::cerr << "Extra ASIO device failed: " << openError << "\n";
                return 23;
            }
            std::cout << "Extra ASIO: " << driver << " at " << extra->getRate() << " Hz, inputs=" << extra->getOpenedInputs() << ", outputs=" << extra->getOpenedOutputs() << "\n";
            extraAsioIds.insert (deviceDef.id);
            extraAsioDevices[deviceDef.id] = std::move (extra);
        }
        std::vector<std::unique_ptr<BusRuntime>> buses;
        std::map<juce::String, int> busIndex;
        for (const auto& def : config.buses)
        {
            auto bus = std::make_unique<BusRuntime>();
            bus->def = def;
            bus->targetGainDb.store (def.gainDb);
            bus->muted.store (def.muted);
            bus->channels = juce::jlimit (1, 32, def.channels);
            busIndex[def.id] = (int) buses.size();
            buses.push_back (std::move (bus));
        }

        std::vector<std::unique_ptr<SourceRuntime>> sources;
        for (const auto& def : config.sources)
        {
            auto rt = std::make_unique<SourceRuntime>();
            rt->def = def;
            rt->targetGainDb.store (def.gainDb);
            rt->muted.store (def.muted);
            rt->channels = (def.type == "asio-in" || def.type == "test") ? juce::jmax (1, (int) def.channels.size()) : 2;
            rt->ring = std::make_unique<nlmatrix::MatrixRing> (rt->channels, kRingFrames);
            if (extraAsioIds.count (def.deviceId) != 0)
                rt->asioInputRing = std::make_unique<nlmatrix::MatrixRing> (rt->channels, kRingFrames);

            if (def.type == "bus")
            {
                const auto it = busIndex.find (def.busId);
                if (it == busIndex.end())
                {
                    std::cerr << "Source " << def.id << " references unknown bus " << def.busId << ".\n";
                    return 7;
                }
                rt->busIndex = it->second;
                rt->channels = buses[(size_t) rt->busIndex]->channels;
            }
            else if (def.type == "test")
            {
                rt->testPhases.assign ((size_t) rt->channels, 0.0);
                rt->testFrequencies.resize ((size_t) rt->channels);
                for (int ch = 0; ch < rt->channels; ++ch)
                    rt->testFrequencies[(size_t) ch] = def.baseFrequency + ch * 40.0;
                rt->sampleRate.store (48000.0, std::memory_order_release);
                rt->online = true;
            }
            else if (def.type == "asio-in")
            {
                for (int ch : def.channels)
                {
                    if (ch < 0 || ch >= asioIns.size())
                    {
                        std::cerr << "Source " << def.id << ": ASIO input " << ch << " out of range.\n";
                        return 7;
                    }
                    rt->asioCompact.push_back (inMap[(size_t) ch]);
                }
            }
            else if (def.type == "loopback")
            {
                rt->loopback = std::make_unique<WasapiLoopbackCapture>();
                auto* ring = rt->ring.get();
                auto* peak = &rt->peak;
                std::string loopError;
                if (rt->loopback->start (std::wstring (def.endpoint.toWideCharPointer()),
                                         [ring, peak] (const float* l, const float* r, int frames)
                                         {
                                             const float* frames2[2] = { l, r }; ring->write (frames2, 2, frames);
                                             float p = 0.0f;
                                             for (int i = 0; i < frames; ++i)
                                                 p = juce::jmax (p, std::abs (l[i]));
                                             if (p > peak->load())
                                                 peak->store (p);
                                         },
                                         loopError))
                {
                    if (rt->loopback->getSampleRate() > 0.0)
                        rt->sampleRate.store (rt->loopback->getSampleRate(),
                                              std::memory_order_release);
                    rt->online = true;
                }
                else
                {
                    rt->loopback->stop();
                    std::cerr << "Source " << def.id << " offline: "
                              << (! loopError.empty() ? loopError : "rate mismatch")
                              << " (will retry)\n";
                    rt->online = false;
                }
            }
            else if (def.type == "process")
            {
                unsigned long pid = findProcessIdForSource (def);

                auto* ring = rt->ring.get();
                auto* peak = &rt->peak;
                rt->processCapture = std::make_unique<ProcessLoopbackCapture>();
                std::string processError;
                if (pid != 0)
                {
                    if (rt->processCapture->start (
                            pid, def.includeProcessTree, asioRate,
                            [ring, peak] (const float* l, const float* r, int frames)
                            {
                                const float* frames2[2] = { l, r }; ring->write (frames2, 2, frames);
                                float p = 0.0f;
                                for (int i = 0; i < frames; ++i)
                                    p = juce::jmax (p, std::abs (l[i]));
                                if (p > peak->load())
                                    peak->store (p);
                            },
                            processError))
                    {
                        rt->sampleRate.store (asioRate, std::memory_order_release);
                        rt->online = true;
                        std::cout << "  process source " << def.id << " -> PID " << pid
                                  << (def.includeProcessTree ? " (tree)" : " (single)") << "\n";
                    }
                    else
                    {
                        std::cerr << "Source " << def.id << " offline: " << processError
                                  << " (will retry)\n";
                        rt->online = false;
                    }
                }
                else
                {
                    std::cerr << "Source " << def.id << " offline: process not found: "
                              << def.process << " (will retry)\n";
                    rt->online = false;
                }
            }
            else if (def.type == "capture")
            {
                const auto name = chooseName (winIns, def.endpoint, "CABLE Output");
                if (name.isEmpty())
                {
                    std::cerr << "Source " << def.id
                              << " offline: capture endpoint not found (engine continues)\n";
                    rt->online = false;
                }
                else if ((rt->captureDevice.reset (wt->createDevice ({}, name)),
                          rt->captureDevice == nullptr))
                {
                    std::cerr << "Source " << def.id
                              << " offline: capture device failed (engine continues)\n";
                    rt->online = false;
                }
                else
                {
                    const auto ins = rt->captureDevice->getInputChannelNames();
                    if ((int) ins.size() != rt->channels && ! ins.isEmpty())
                    {
                        rt->channels = juce::jlimit (1, 32, ins.size());
                        rt->ring = std::make_unique<nlmatrix::MatrixRing> (rt->channels, kRingFrames);
                    }
                    rt->captureCallback = std::make_unique<CaptureCallback> (*rt->ring, rt->peak,
                                                                             rt->sampleRate);
                    juce::BigInteger mask;
                    mask.setRange (0, ins.size(), true);
                    juce::BigInteger noOut;
                    const auto err = rt->captureDevice->open (mask, noOut, asioRate, 512);
                    if (err.isNotEmpty())
                    {
                        std::cerr << "Source " << def.id
                                  << " offline: capture open failed: " << err
                                  << " (engine continues)\n";
                        rt->captureDevice.reset();
                        rt->online = false;
                    }
                }
            }
            else if (def.type == "driver")
            {
                // The driver may be absent (main PC without test signing, or
                // the virtual endpoint temporarily missing). Keep the node
                // offline and reconnect in the pump thread instead of aborting
                // the whole router.
                rt->driverStop.store (false, std::memory_order_relaxed);
                auto* source = rt.get();
                rt->driverThread = std::thread ([source]
                {
                    std::vector<uint8_t> raw (16384);
                    std::vector<float> left (4096), right (4096);
                    NLMATRIX_STREAM_INFO format {};
                    bool reportedOffline = false;
                    auto nextFormatCheck = std::chrono::steady_clock::now();
                    auto nextWake = std::chrono::steady_clock::now();
                    while (! source->driverStop.load (std::memory_order_acquire))
                    {
                        if (source->driver == nullptr)
                        {
                            auto client = std::make_unique<NlDriverClient>();
                            std::string error;
                            if (! client->open (error))
                            {
                                source->online.store (false, std::memory_order_relaxed);
                                if (! reportedOffline)
                                {
                                    std::cerr << "Source " << source->def.id
                                              << " offline (NLMatrixAudio driver): " << error
                                              << " — retrying\n";
                                    reportedOffline = true;
                                }
                                std::this_thread::sleep_for (std::chrono::milliseconds (500));
                                continue;
                            }
                            NLMATRIX_INFO info {};
                            if (! client->getInfo (info, error))
                            {
                                client->close();
                                source->online.store (false);
                                if (! reportedOffline)
                                {
                                    std::cerr << "Source " << source->def.id
                                              << " offline (driver info): " << error
                                              << " — retrying\n";
                                    reportedOffline = true;
                                }
                                std::this_thread::sleep_for (std::chrono::milliseconds (500));
                                continue;
                            }
                            const int endpoint = juce::jlimit (0, NLMATRIX_MAX_ENDPOINTS - 1,
                                                               source->def.driverEndpoint);
                            if ((unsigned long) endpoint >= info.EndpointCount)
                            {
                                client->close();
                                source->online.store (false);
                                if (! reportedOffline)
                                {
                                    std::cerr << "Source " << source->def.id
                                              << " offline: driver endpoint " << endpoint
                                              << " not present\n";
                                    reportedOffline = true;
                                }
                                std::this_thread::sleep_for (std::chrono::milliseconds (500));
                                continue;
                            }
                            format = info.Endpoints[endpoint].Render;
                            if (format.SampleRate > 0)
                                source->sampleRate.store ((double) format.SampleRate,
                                                         std::memory_order_release);
                            source->driver = std::move (client);
                            source->online.store (true);
                            reportedOffline = false;
                            std::cout << "Source " << source->def.id
                                      << " driver reconnected: " << format.SampleRate
                                      << " Hz " << format.Channels << " ch\n";
                            nextFormatCheck = std::chrono::steady_clock::now()
                                              + std::chrono::milliseconds (500);
                        }

                        const auto loopNow = std::chrono::steady_clock::now();
                        if (loopNow >= nextFormatCheck)
                        {
                            nextFormatCheck = loopNow + std::chrono::milliseconds (500);
                            // Windows can renegotiate the endpoint format when an
                            // application opens/closes it.  Keep the converter's
                            // declared source rate in sync with the transport.
                            const int ep = juce::jlimit (0, NLMATRIX_MAX_ENDPOINTS - 1,
                                                        source->def.driverEndpoint);
                            NLMATRIX_INFO liveInfo {};
                            std::string infoError;
                            if (source->driver->getInfo (liveInfo, infoError)
                                && (unsigned long) ep < liveInfo.EndpointCount)
                            {
                                const auto fresh = liveInfo.Endpoints[ep].Render;
                                if (fresh.SampleRate != format.SampleRate
                                    || fresh.Channels != format.Channels
                                    || fresh.BitsPerSample != format.BitsPerSample)
                                {
                                    format = fresh;
                                    if (format.SampleRate > 0)
                                        source->sampleRate.store ((double) format.SampleRate,
                                                                 std::memory_order_release);
                                }
                            }
                        }

                        std::string error;
                        const int bytes = source->driver->readRender (
                            source->def.driverEndpoint, raw.data(), (int) raw.size(), error);
                        if (bytes < 0)
                        {
                            source->online.store (false);
                            source->driver->close();
                            source->driver.reset();
                            std::this_thread::sleep_for (std::chrono::milliseconds (500));
                            continue;
                        }
                        if (bytes > 0)
                        {
                            const int frames = nlRawToStereo (raw.data(), bytes, format,
                                                              left.data(), right.data());
                            if (frames > 0)
                            {
                                const float* frames2[2] = { left.data(), right.data() }; source->ring->write (frames2, 2, frames);
                                float p = 0.0f;
                                for (int i = 0; i < frames; ++i)
                                    p = juce::jmax (p, std::abs (left[(size_t) i]));
                                if (p > source->peak.load())
                                    source->peak.store (p);
                            }
                        }
                        std::this_thread::sleep_for (std::chrono::milliseconds (2));
                    }
                });

                std::cout << "  driver source " << def.id << " <- NL Return (hot-plug)\n";
            }
            else
            {
                std::cerr << "Unknown source type: " << def.type << "\n";
                return 13;
            }

            sources.push_back (std::move (rt));
        }

        std::vector<std::unique_ptr<DestinationRuntime>> destinations;
        for (const auto& def : config.destinations)
        {
            auto rt = std::make_unique<DestinationRuntime>();
            rt->def = def;
            rt->targetGainDb.store (def.gainDb);
            rt->muted.store (def.muted);
            rt->channels = (def.type == "asio-out" || def.type == "null") ? juce::jmax (1, (int) def.channels.size()) : 2;
            rt->ring = std::make_unique<nlmatrix::MatrixRing> (rt->channels, kRingFrames);
            if (extraAsioIds.count (def.deviceId) != 0)
                rt->asioOutputRing = std::make_unique<nlmatrix::MatrixRing> (rt->channels, kRingFrames);

            if (def.type == "asio-out")
            {
                for (int ch : def.channels)
                {
                    if (ch < 0 || ch >= asioOuts.size())
                    {
                        std::cerr << "Destination " << def.id << ": ASIO output " << ch << " out of range.\n";
                        return 14;
                    }
                    rt->asioCompact.push_back (outMap[(size_t) ch]);
                }
            }
            else if (def.type == "null")
            {
                rt->channels = juce::jmax (1, (int) def.channels.size());
                rt->online = true;
            }
            else if (def.type == "render")
            {
                const auto name = chooseName (winOuts, def.endpoint, "CABLE Input");
                if (name.isNotEmpty())
                {
                    rt->renderDevice.reset (wt->createDevice (name, {}));
                    if (rt->renderDevice != nullptr)
                    {
                        const auto outs = rt->renderDevice->getOutputChannelNames();
                        if ((int) outs.size() != rt->channels && ! outs.isEmpty())
                        {
                            rt->channels = juce::jlimit (1, 32, outs.size());
                            rt->ring = std::make_unique<nlmatrix::MatrixRing> (rt->channels, kRingFrames);
                        }
                        rt->renderCallback = std::make_unique<RenderCallback> (*rt->ring, rt->peak,
                                                                               asioRate);
                        juce::BigInteger mask;
                        mask.setRange (0, juce::jmin (32, outs.size()), true);
                        juce::BigInteger noIn;
                        const auto err = rt->renderDevice->open (noIn, mask, asioRate, 512);
                        if (err.isEmpty())
                            rt->online = true;
                        else
                        {
                            std::cerr << "Destination " << def.id
                                      << " offline: " << err << " (will retry)\n";
                            rt->renderDevice.reset();
                            rt->online = false;
                        }
                    }
                }
                if (rt->renderDevice == nullptr)
                {
                    std::cerr << "Destination " << def.id << " offline: endpoint not found (will retry)\n";
                    rt->online = false;
                }
            }
            else if (def.type == "driver")
            {
                rt->driverStop.store (false, std::memory_order_relaxed);
                auto* destination = rt.get();
                rt->driverThread = std::thread ([destination, rate = asioRate]
                {
                   #if JUCE_WINDOWS
                    SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
                    timeBeginPeriod (1);
                   #endif
                    std::vector<float> left (4096), right (4096);
                    std::vector<uint8_t> raw;
                    NLMATRIX_STREAM_INFO format {};
                    nlmatrix::MatrixRateConverter converter (2);
                    bool primed = false;
                    bool reportedOffline = false;
                    auto nextFormatCheck = std::chrono::steady_clock::now();
                    auto nextWake = std::chrono::steady_clock::now();
                    while (! destination->driverStop.load (std::memory_order_acquire))
                    {
                        if (destination->driver == nullptr)
                        {
                            auto client = std::make_unique<NlDriverClient>();
                            std::string error;
                            if (! client->open (error))
                            {
                                destination->online.store (false, std::memory_order_relaxed);
                                if (! reportedOffline)
                                {
                                    std::cerr << "Destination " << destination->def.id
                                              << " offline (NLMatrixAudio driver): " << error
                                              << " — retrying\n";
                                    reportedOffline = true;
                                }
                                std::this_thread::sleep_for (std::chrono::milliseconds (500));
                                continue;
                            }
                            NLMATRIX_INFO info {};
                            if (! client->getInfo (info, error))
                            {
                                client->close();
                                destination->online.store (false);
                                std::this_thread::sleep_for (std::chrono::milliseconds (500));
                                continue;
                            }
                            const int endpoint = juce::jlimit (0, NLMATRIX_MAX_ENDPOINTS - 1,
                                                               destination->def.driverEndpoint);
                            if ((unsigned long) endpoint >= info.EndpointCount)
                            {
                                client->close();
                                destination->online.store (false);
                                if (! reportedOffline)
                                {
                                    std::cerr << "Destination " << destination->def.id
                                              << " offline: driver endpoint " << endpoint
                                              << " not present\n";
                                    reportedOffline = true;
                                }
                                std::this_thread::sleep_for (std::chrono::milliseconds (500));
                                continue;
                            }
                            format = info.Endpoints[endpoint].Capture;
                            destination->sampleRate.store ((double) format.SampleRate,
                                                           std::memory_order_release);
                            converter.reset (2);
                            destination->driver = std::move (client);
                            destination->online.store (true);
                            reportedOffline = false;
                            std::cout << "Destination " << destination->def.id
                                      << " driver reconnected: " << format.SampleRate
                                      << " Hz " << format.Channels << " ch\n";
                            nextFormatCheck = std::chrono::steady_clock::now()
                                              + std::chrono::milliseconds (500);
                        }

                        const auto loopNow = std::chrono::steady_clock::now();
                        if (loopNow >= nextFormatCheck)
                        {
                            nextFormatCheck = loopNow + std::chrono::milliseconds (500);
                            const int ep = juce::jlimit (0, NLMATRIX_MAX_ENDPOINTS - 1,
                                                        destination->def.driverEndpoint);
                            NLMATRIX_INFO liveInfo {};
                            std::string infoError;
                            if (destination->driver->getInfo (liveInfo, infoError)
                                && (unsigned long) ep < liveInfo.EndpointCount)
                            {
                                const auto fresh = liveInfo.Endpoints[ep].Capture;
                                if (fresh.SampleRate != format.SampleRate
                                    || fresh.Channels != format.Channels
                                    || fresh.BitsPerSample != format.BitsPerSample)
                                {
                                    format = fresh;
                                    destination->sampleRate.store ((double) format.SampleRate,
                                                                   std::memory_order_release);
                                    converter.reset (2);
                                }
                            }
                        }

                        const double destRate = format.SampleRate > 0
                                                    ? (double) format.SampleRate : rate;
                        const int frames = juce::jlimit (
                            1, (int) left.size(),
                            (int) std::lround (destRate * 0.010));
                        const int targetFrames = juce::jmax (256, (int) std::lround (destRate * 0.030));
                        if (! primed)
                        {
                            if (destination->ring->available() < targetFrames)
                            {
                                std::this_thread::sleep_for (std::chrono::milliseconds (2));
                                continue;
                            }
                            primed = true;
                        }
                        std::vector<float*> destinationPointers { left.data(), right.data() }; converter.read (*destination->ring, destinationPointers.data(), 2, frames,
                                        rate, destRate, targetFrames);
                        nlStereoToRaw (left.data(), right.data(), frames, format, raw);
                        std::string error;
                        if (! destination->driver->writeCapture (
                                destination->def.driverEndpoint, raw.data(),
                                (int) raw.size(), error))
                        {
                            destination->driver->close();
                            destination->driver.reset();
                            destination->online.store (false);
                            std::this_thread::sleep_for (std::chrono::milliseconds (500));
                            continue;
                        }
                        destination->lastDriverWriteMs.store ((int64_t) juce::Time::getMillisecondCounterHiRes());
                        destination->driverUnderruns.store (converter.getUnderruns());
                        nextWake += std::chrono::milliseconds (10);
                        if (std::chrono::steady_clock::now() > nextWake + std::chrono::milliseconds (20))
                            nextWake = std::chrono::steady_clock::now();
                        std::this_thread::sleep_until (nextWake);
                    }
                });

                std::cout << "  driver destination " << def.id << " -> NL Mic (hot-plug)\n";
            }
            else
            {
                std::cerr << "Unknown destination type: " << def.type << "\n";
                return 18;
            }

            destinations.push_back (std::move (rt));
        }

        // Bind each extra ASIO device to the sources/destinations that name it
        // and start its I/O callback.  Its rings are drained by the master graph.
        for (auto& [deviceId, extra] : extraAsioDevices)
        {
            std::vector<SourceRuntime*> extraSources;
            std::vector<DestinationRuntime*> extraDestinations;
            for (auto& source : sources)
                if (source->def.deviceId == deviceId)
                    extraSources.push_back (source.get());
            for (auto& destination : destinations)
                if (destination->def.deviceId == deviceId)
                    extraDestinations.push_back (destination.get());
            extra->attach (std::move (extraSources), std::move (extraDestinations), asioRate);
            extra->start();
            if (! extra->isPlaying())
            {
                std::cerr << "Extra ASIO device did not start.\n";
                return 24;
            }
        }
        // Process sources are hot-pluggable: keep the route and retry when the
        // application starts, exits or restarts. The ring is zero-filled while
        // offline, so the rest of the graph keeps running.
        for (auto& s : sources)
        {
            if (s->def.type != "process")
                continue;
            auto* source = s.get();
            source->processWatchStop.store (false, std::memory_order_relaxed);
            source->processWatch = std::thread ([source, rate = asioRate]
            {
                const auto sink = [ring = source->ring.get(), peak = &source->peak]
                    (const float* l, const float* r, int frames)
                {
                    const float* frames2[2] = { l, r }; ring->write (frames2, 2, frames);
                    float p = 0.0f;
                    for (int i = 0; i < frames; ++i)
                        p = juce::jmax (p, std::abs (l[i]));
                    if (p > peak->load())
                        peak->store (p);
                };

                int failures = 0;
                while (! source->processWatchStop.load (std::memory_order_acquire))
                {
                    if (! source->enabled.load (std::memory_order_acquire))
                    {
                        if (source->processCapture != nullptr)
                        {
                            source->processCapture->stop();
                            source->processCapture.reset();
                        }
                        source->online.store (false, std::memory_order_relaxed);
                        juce::Thread::sleep (200);
                        continue;
                    }
                    const bool running = source->processCapture != nullptr
                                         && source->processCapture->isRunning();
                    source->online.store (running, std::memory_order_relaxed);
                    if (! running)
                    {
                        unsigned long pid = findProcessIdForSource (source->def);

                        if (pid != 0)
                        {
                            auto capture = std::make_unique<ProcessLoopbackCapture>();
                            std::string error;
                            if (capture->start (pid, source->def.includeProcessTree,
                                                rate, sink, error))
                            {
                                source->processCapture = std::move (capture);
                                source->online.store (true, std::memory_order_relaxed);
                                failures = 0;
                                std::cout << "Source " << source->def.id
                                          << " reconnected to PID " << pid << "\n";
                            }
                            else if (++failures % 10 == 1)
                            {
                                std::cerr << "Source " << source->def.id
                                          << " still offline: " << error << "\n";
                            }
                        }
                        else if (++failures % 10 == 1)
                        {
                            std::cerr << "Source " << source->def.id
                                      << " still waiting for process "
                                      << source->def.process << "\n";
                        }
                    }
                    else
                    {
                        failures = 0;
                    }
                    juce::Thread::sleep (500);
                }
            });
        }

        // Endpoint loopback sources are hot-pluggable too (USB removal,
        // default-device changes, endpoint restart). Their ring is zero-filled
        // while offline and routing continues unchanged.
        for (auto& s : sources)
        {
            if (s->def.type != "loopback")
                continue;
            auto* source = s.get();
            source->processWatchStop.store (false, std::memory_order_relaxed);
            source->processWatch = std::thread ([source, rate = asioRate]
            {
                const auto sink = [ring = source->ring.get(), peak = &source->peak]
                    (const float* l, const float* r, int frames)
                {
                    const float* frames2[2] = { l, r }; ring->write (frames2, 2, frames);
                    float p = 0.0f;
                    for (int i = 0; i < frames; ++i)
                        p = juce::jmax (p, std::abs (l[i]));
                    if (p > peak->load())
                        peak->store (p);
                };

                int failures = 0;
                while (! source->processWatchStop.load (std::memory_order_acquire))
                {
                    const bool running = source->loopback != nullptr
                                         && source->loopback->isRunning();
                    source->online.store (running, std::memory_order_relaxed);
                    if (! running)
                    {
                        auto capture = std::make_unique<WasapiLoopbackCapture>();
                        std::string error;
                        if (capture->start (
                                std::wstring (source->def.endpoint.toWideCharPointer()),
                                sink, error)
                            && std::abs (capture->getSampleRate() - rate) <= 0.5)
                        {
                            source->loopback = std::move (capture);
                            source->online.store (true, std::memory_order_relaxed);
                            failures = 0;
                            std::cout << "Source " << source->def.id
                                      << " loopback reconnected: "
                                      << source->def.endpoint << "\n";
                        }
                        else if (++failures % 10 == 1)
                        {
                            std::cerr << "Source " << source->def.id
                                      << " loopback still offline: "
                                      << (error.empty() ? "rate mismatch" : error) << "\n";
                        }
                    }
                    else
                    {
                        failures = 0;
                    }
                    juce::Thread::sleep (500);
                }
            });
        }

        // Render endpoints are hot-pluggable as well. The destination ring is
        // drained even while offline, so reconnecting does not replay stale audio.
        for (auto& d : destinations)
        {
            if (d->def.type != "render")
                continue;
            auto* destination = d.get();
            auto* deviceType = wt.get();
            destination->processWatchStop.store (false, std::memory_order_relaxed);
            destination->processWatch = std::thread ([destination, deviceType, rate = asioRate]
            {
                // Let the initial start path finish before the watchdog checks.
                juce::Thread::sleep (1000);
                int failures = 0;
                while (! destination->processWatchStop.load (std::memory_order_acquire))
                {
                    const bool running = destination->renderDevice != nullptr
                                         && destination->renderDevice->isPlaying();
                    destination->online.store (running, std::memory_order_relaxed);
                    if (! running)
                    {
                        if (destination->renderDevice != nullptr)
                        {
                            destination->renderDevice->stop();
                            destination->renderDevice->close();
                            destination->renderDevice.reset();
                        }
                        deviceType->scanForDevices();
                        const auto names = deviceType->getDeviceNames (false);
                        const auto name = chooseName (names, destination->def.endpoint,
                                                      "CABLE Input");
                        if (name.isNotEmpty())
                        {
                            auto device = std::unique_ptr<juce::AudioIODevice> (
                                deviceType->createDevice (name, {}));
                            if (device != nullptr)
                            {
                                auto callback = std::make_unique<RenderCallback> (
                                    *destination->ring, destination->peak, rate);
                                const auto outs = device->getOutputChannelNames();
                                juce::BigInteger mask;
                                mask.setRange (0, juce::jmin (2, outs.size()), true);
                                juce::BigInteger noIn;
                                const auto err = device->open (noIn, mask, rate, 512);
                                if (err.isEmpty())
                                {
                                    device->start (callback.get());
                                    if (device->isPlaying())
                                    {
                                        destination->renderDevice = std::move (device);
                                        destination->renderCallback = std::move (callback);
                                        destination->online.store (true);
                                        failures = 0;
                                        std::cout << "Destination " << destination->def.id
                                                  << " render reconnected: " << name << "\n";
                                    }
                                }
                                else if (++failures % 10 == 1)
                                {
                                    std::cerr << "Destination " << destination->def.id
                                              << " render still offline: " << err << "\n";
                                }
                            }
                        }
                        else if (++failures % 10 == 1)
                        {
                            std::cerr << "Destination " << destination->def.id
                                      << " still waiting for render endpoint "
                                      << destination->def.endpoint << "\n";
                        }
                    }
                    else
                    {
                        failures = 0;
                    }
                    juce::Thread::sleep (500);
                }
            });
        }

        std::map<juce::String, int> sourceIndex;
        for (int i = 0; i < (int) sources.size(); ++i)
            sourceIndex[sources[(size_t) i]->def.id] = i;
        std::map<juce::String, int> destinationIndex;
        for (int i = 0; i < (int) destinations.size(); ++i)
            destinationIndex[destinations[(size_t) i]->def.id] = i;

        GraphSnapshot initialGraph;
        juce::String graphError;
        if (! buildRoutingSnapshot (config, sources, sourceIndex, destinationIndex,
                                    busIndex, initialGraph, graphError))
        {
            std::cerr << "Routing graph invalid: " << graphError << "\n";
            return 19;
        }

        std::atomic<const GraphSnapshot*> graph { nullptr };
        std::vector<std::unique_ptr<GraphSnapshot>> retiredGraphs;
        retiredGraphs.push_back (std::make_unique<GraphSnapshot> (std::move (initialGraph)));
        graph.store (retiredGraphs.back().get(), std::memory_order_release);
        for (int i = 0; i < (int) sources.size() && i < (int) config.sources.size(); ++i)
        {
            sources[(size_t) i]->enabled.store (config.sources[(size_t) i].enabled,
                                               std::memory_order_release);
            sources[(size_t) i]->targetGainDb.store (config.sources[(size_t) i].gainDb);
            sources[(size_t) i]->muted.store (config.sources[(size_t) i].muted);
        }
        for (int i = 0; i < (int) destinations.size() && i < (int) config.destinations.size(); ++i)
        {
            destinations[(size_t) i]->enabled.store (config.destinations[(size_t) i].enabled);
            destinations[(size_t) i]->targetGainDb.store (config.destinations[(size_t) i].gainDb);
            destinations[(size_t) i]->muted.store (config.destinations[(size_t) i].muted);
        }
        for (int i = 0; i < (int) buses.size() && i < (int) config.buses.size(); ++i)
        {
            buses[(size_t) i]->targetGainDb.store (config.buses[(size_t) i].gainDb);
            buses[(size_t) i]->muted.store (config.buses[(size_t) i].muted);
        }

        std::vector<SourceRuntime*> sourcePtrs;
        for (auto& s : sources)
            sourcePtrs.push_back (s.get());
        std::vector<DestinationRuntime*> destinationPtrs;
        for (auto& d : destinations)
            destinationPtrs.push_back (d.get());
        std::vector<BusRuntime*> busPtrs;
        for (auto& b : buses)
            busPtrs.push_back (b.get());

        RouterCallback callback (sourcePtrs, destinationPtrs, busPtrs,
                                 retiredGraphs.back()->busOrder, graph);

        for (auto& s : sources)
            if (s->captureDevice != nullptr)
            {
                s->captureDevice->start (s->captureCallback.get());
                if (! s->captureDevice->isPlaying())
                {
                    std::cerr << "Source " << s->def.id << " did not start.\n";
                    return 19;
                }
            }

        for (auto& d : destinations)
            if (d->renderDevice != nullptr)
            {
                d->renderDevice->start (d->renderCallback.get());
                if (! d->renderDevice->isPlaying())
                {
                    std::cerr << "Destination " << d->def.id
                              << " offline: render did not start (will retry)\n";
                    d->renderDevice->close();
                    d->renderDevice.reset();
                    d->online = false;
                }
            }

        asioDevice->start (&callback);
        if (! asioDevice->isPlaying())
        {
            std::cerr << "ASIO device did not start.\n";
            return 21;
        }

        std::cout << "Running. Ctrl+C to stop.";
        if (seconds > 0.0)
            std::cout << " Auto-stop after " << seconds << " s.";
        std::cout << "\n";

        std::cout.setf (std::ios::unitbuf);
        std::ofstream telemetryOutput;
        if (telemetryPath.isNotEmpty())
        {
            telemetryOutput.open (telemetryPath.toRawUTF8(), std::ios::out | std::ios::trunc);
            if (! telemetryOutput.is_open())
                std::cerr << "Telemetry file unavailable: " << telemetryPath << "\n";
        }
        const double start = juce::Time::getMillisecondCounterHiRes();
        double nextMeter = start + 1000.0;
        double nextTelemetry = start;
        double nextConfigCheck = start + 200.0;
        double idleSince = -1.0;
        juce::int64 lastConfigMod = configFile.getLastModificationTime().toMilliseconds();
        Config currentConfig = config;

        while (! gStop)
        {
            juce::Thread::sleep (50);
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (seconds > 0.0 && now - start >= seconds * 1000.0)
                break;
            if (idleExitSeconds > 0.0)
            {
                const auto* liveSnapshot = graph.load (std::memory_order_acquire);
                const bool graphActive = liveSnapshot != nullptr && liveSnapshot->active;
                if (graphActive)
                    idleSince = -1.0;
                else if (idleSince < 0.0)
                    idleSince = now;
                else if (now - idleSince >= idleExitSeconds * 1000.0)
                {
                    std::cout << "Idle release: no active routes for "
                              << idleExitSeconds << " s; closing devices and exiting.\n";
                    break;
                }
            }

            if (now >= nextConfigCheck)
            {
                nextConfigCheck = now + 200.0;
                const auto mod = configFile.getLastModificationTime().toMilliseconds();
                if (mod != lastConfigMod)
                {
                    Config nextConfig;
                    juce::String loadError;
                    if (loadConfig (configFile, nextConfig, loadError))
                    {
                        if (! sameTopology (currentConfig, nextConfig))
                        {
                            std::cout << "Live config: topology changed; restart required "
                                         "for added/removed sources or destinations.\n";
                        }
                        else
                        {
                            GraphSnapshot nextGraph;
                            juce::String liveGraphError;
                            if (buildRoutingSnapshot (nextConfig, sources, sourceIndex,
                                                      destinationIndex, busIndex,
                                                      nextGraph, liveGraphError))
                            {
                                retiredGraphs.push_back (
                                    std::make_unique<GraphSnapshot> (std::move (nextGraph)));
                                graph.store (retiredGraphs.back().get(),
                                             std::memory_order_release);
                                currentConfig = nextConfig;
                                for (int i = 0; i < (int) sources.size() && i < (int) nextConfig.sources.size(); ++i)
                                {
                                    sources[(size_t) i]->enabled.store (nextConfig.sources[(size_t) i].enabled,
                                                                       std::memory_order_release);
                                    sources[(size_t) i]->targetGainDb.store (nextConfig.sources[(size_t) i].gainDb);
                                    sources[(size_t) i]->muted.store (nextConfig.sources[(size_t) i].muted);
                                }
                                for (int i = 0; i < (int) destinations.size() && i < (int) nextConfig.destinations.size(); ++i)
                                {
                                    destinations[(size_t) i]->enabled.store (nextConfig.destinations[(size_t) i].enabled);
                                    destinations[(size_t) i]->targetGainDb.store (nextConfig.destinations[(size_t) i].gainDb);
                                    destinations[(size_t) i]->muted.store (nextConfig.destinations[(size_t) i].muted);
                                }
                                for (int i = 0; i < (int) buses.size() && i < (int) nextConfig.buses.size(); ++i)
                                {
                                    buses[(size_t) i]->targetGainDb.store (nextConfig.buses[(size_t) i].gainDb);
                                    buses[(size_t) i]->muted.store (nextConfig.buses[(size_t) i].muted);
                                }
                                std::cout << "Live routing update: "
                                          << nextConfig.routes.size() << " route(s).\n";
                            }
                            else
                            {
                                std::cerr << "Live routing rejected: " << liveGraphError << "\n";
                            }
                        }
                        lastConfigMod = mod;
                    }
                    else
                    {
                        std::cerr << "Live config unreadable, keeping previous routing: "
                                  << loadError << "\n";
                    }
                }
            }

            if (now >= nextTelemetry)
            {
                nextTelemetry = now + 100.0;
                auto root = juce::var (new juce::DynamicObject());
                auto* object = root.getDynamicObject();
                object->setProperty ("type", "meters");
                object->setProperty ("callbacks", (juce::int64) callback.callbacks.load());
                object->setProperty ("framesProcessed", (juce::int64) callback.framesProcessed.load());
                const double callbackAge = callback.lastCallbackMs.load() > 0.0
                    ? juce::Time::getMillisecondCounterHiRes() - callback.lastCallbackMs.load() : 1.0e9;
                object->setProperty ("lastCallbackAgeMs", callbackAge);
                object->setProperty ("engineHealthy", callbackAge < 500.0 && ! callback.error.load());
                object->setProperty ("asioError", callback.error.load());
                object->setProperty ("engineRate", asioRate);
                object->setProperty ("engineBuffer", asioDevice->getCurrentBufferSizeSamples());
                object->setProperty ("asio", asioName);
                juce::Array<juce::var> nodes;
                auto addNode = [&nodes] (const juce::String& id, const juce::String& kind,
                                         std::atomic<float>& l, std::atomic<float>& r,
                                         std::atomic<bool>& clipped, bool online,
                                         std::array<std::atomic<float>, 32>& channels, int numChannels,
                                         juce::uint64 dropped, juce::uint64 underruns)
                {
                    auto node = juce::var (new juce::DynamicObject());
                    auto* o = node.getDynamicObject();
                    o->setProperty ("id", id);
                    o->setProperty ("kind", kind);
                    o->setProperty ("l", juce::Decibels::gainToDecibels (l.exchange (0.0f), -100.0f));
                    o->setProperty ("r", juce::Decibels::gainToDecibels (r.exchange (0.0f), -100.0f));
                    o->setProperty ("clip", clipped.exchange (false));
                    o->setProperty ("online", online);
                    juce::Array<juce::var> channelArray;
                    for (int ch = 0; ch < juce::jlimit (0, 32, numChannels); ++ch)
                        channelArray.add ((double) channels[(size_t) ch].exchange (0.0f));
                    o->setProperty ("channels", channelArray);
                    o->setProperty ("dropped", (juce::int64) dropped);
                    o->setProperty ("underruns", (juce::int64) underruns);
                    nodes.add (node);
                };
                for (auto& s : sources)
                    addNode (s->def.id, s->def.type == "bus" ? "bus" : "source",
                             s->peakL, s->peakR, s->clipped,
                             s->online.load (std::memory_order_relaxed), s->channelPeak, s->channels,
                             s->ring != nullptr ? s->ring->getDroppedFrames() : 0,
                             s->converter.getUnderruns());
                for (auto& b : buses)
                    addNode (b->def.id, "bus", b->peakL, b->peakR, b->clipped, true, b->channelPeak, b->channels, 0, 0);
                for (auto& d : destinations)
                    addNode (d->def.id, "destination", d->peakL, d->peakR,
                             d->clipped, d->online.load (std::memory_order_relaxed), d->channelPeak, d->channels,
                             d->ring != nullptr ? d->ring->getDroppedFrames() : 0,
                             (d->asioOutputConverter != nullptr ? d->asioOutputConverter->getUnderruns() : 0)
                             + d->driverUnderruns.load());
                object->setProperty ("nodes", nodes);
                const auto json = juce::JSON::toString (root, true);
                if (telemetryOutput.is_open())
                {
                    telemetryOutput << "METERS " << json << "\n";
                    telemetryOutput.flush();
                }
                if (telemetryPath.isEmpty())
                    std::cout << "METERS " << json << "\n";
            }

            if (now >= nextMeter && telemetryPath.isEmpty())
            {
                nextMeter += 1000.0;
                std::cout << "callbacks " << callback.callbacks.load();
                for (auto& s : sources)
                    std::cout << " | " << s->def.id << " "
                              << juce::String (juce::Decibels::gainToDecibels (
                                     s->peak.exchange (0.0f), -100.0f), 1);
                for (auto& d : destinations)
                    std::cout << " | " << d->def.id << " "
                              << juce::String (juce::Decibels::gainToDecibels (
                                     d->peak.exchange (0.0f), -100.0f), 1);
                for (auto& b : buses)
                    std::cout << " | bus:" << b->def.id << " "
                              << juce::String (juce::Decibels::gainToDecibels (
                                     b->peak.exchange (0.0f), -100.0f), 1);
                std::cout << "\n";
            }
        }

        for (auto& [deviceId, extra] : extraAsioDevices)
        {
            juce::ignoreUnused (deviceId);
            extra->stop();
        }
        asioDevice->stop();
        asioDevice->close();
        for (auto& s : sources)
        {
            s->driverStop.store (true, std::memory_order_release);
            if (s->driverThread.joinable()) s->driverThread.join();
            if (s->driver != nullptr) s->driver->close();
            if (s->loopback != nullptr) s->loopback->stop();
            s->processWatchStop.store (true, std::memory_order_release);
            if (s->processWatch.joinable()) s->processWatch.join();
            if (s->processCapture != nullptr) s->processCapture->stop();
            if (s->captureDevice != nullptr) { s->captureDevice->stop(); s->captureDevice->close(); }
        }
        for (auto& d : destinations)
        {
            d->driverStop.store (true, std::memory_order_release);
            if (d->driverThread.joinable()) d->driverThread.join();
            if (d->driver != nullptr) d->driver->close();
            d->processWatchStop.store (true, std::memory_order_release);
            if (d->processWatch.joinable()) d->processWatch.join();
            if (d->renderDevice != nullptr) { d->renderDevice->stop(); d->renderDevice->close(); }
        }

        std::cout << "Stopped.\n";
        return 0;
    }

    // Offline regression for the rate converter used between device-clocked
    // rings and the ASIO graph.  It feeds a known 1 kHz / 2 kHz stereo tone at
    // the source rate and measures the produced frequency at the destination
    // rate; no audio device or ASIO driver is required.
    int runResamplerSelfTest()
    {
        struct Case { double sourceRate; double destinationRate; };
        const Case cases[] = { { 96000.0, 48000.0 }, { 44100.0, 48000.0 },
                               { 48000.0, 96000.0 }, { 48000.0, 48000.0 } };
        bool allPass = true;
        for (const auto& c : cases)
        {
            StereoRing ring (1 << 16);
            StereoRateConverter converter;
            const int sourceBlock = (int) std::lround (c.sourceRate * 0.010);
            const int destBlock = (int) std::lround (c.destinationRate * 0.010);
            const int target = (int) std::lround (c.sourceRate * 0.030);
            std::vector<float> left ((size_t) sourceBlock), right ((size_t) sourceBlock);
            std::vector<float> outL ((size_t) destBlock), outR ((size_t) destBlock);
            double sourcePhase = 0.0;
            auto writeBlock = [&]
            {
                for (int i = 0; i < sourceBlock; ++i)
                {
                    left[(size_t) i] = 0.25f * (float) std::sin (sourcePhase);
                    right[(size_t) i] = 0.25f * (float) std::sin (sourcePhase * 2.0);
                    sourcePhase += 2.0 * juce::MathConstants<double>::pi * 1000.0 / c.sourceRate;
                }
                ring.write (left.data(), right.data(), sourceBlock);
            };
            for (int i = 0; i < target / sourceBlock + 2; ++i)
                writeBlock();

            int64_t risingL = 0, risingR = 0;
            int measuredFrames = 0;
            float lastL = 0.0f, lastR = 0.0f;
            const int blocksToMeasure = 100;
            for (int block = 0; block < blocksToMeasure + 10; ++block)
            {
                writeBlock();
                converter.read (ring, outL.data(), outR.data(), destBlock,
                                c.sourceRate, c.destinationRate, target);
                if (block < 10)
                {
                    lastL = outL.back();
                    lastR = outR.back();
                    continue;
                }
                for (int i = 0; i < destBlock; ++i)
                {
                    if (lastL <= 0.0f && outL[(size_t) i] > 0.0f) ++risingL;
                    if (lastR <= 0.0f && outR[(size_t) i] > 0.0f) ++risingR;
                    lastL = outL[(size_t) i];
                    lastR = outR[(size_t) i];
                }
                measuredFrames += destBlock;
            }
            const double seconds = measuredFrames / c.destinationRate;
            const double freqL = risingL / seconds;
            const double freqR = risingR / seconds;
            const bool pass = std::abs (freqL - 1000.0) < 30.0
                              && std::abs (freqR - 2000.0) < 60.0;
            allPass = allPass && pass;
            std::cout << (pass ? "PASS " : "FAIL ")
                      << "resampler " << c.sourceRate << " -> " << c.destinationRate
                      << " L=" << juce::String (freqL, 1) << " Hz"
                      << " R=" << juce::String (freqR, 1) << " Hz\n";
        }
        return allPass ? 0 : 1;
    }

    // Real-time, fully synthetic N×M matrix regression.  Four independent
    // sources (8 channels each, different rates) feed four 8-channel
    // destinations through explicit channel rotations; the ASIO/hardware path
    // is intentionally not touched.  This validates the channel matrix,
    // per-channel SRC, drift trim, overflow/underflow and channel isolation.
    int runMatrixStressTest (double seconds)
    {
#if JUCE_WINDOWS
        struct TimerResolution
        {
            TimerResolution() { timeBeginPeriod (1); }
            ~TimerResolution() { timeEndPeriod (1); }
        } timerResolution;
#endif
        constexpr int kChannels = 8;
        constexpr int kSources = 4;
        constexpr int kDestinations = 4;
        constexpr int kGraphRate = 48000;
        constexpr int kBlock = 256;
        constexpr int kCapacity = 1 << 16;
        const double sourceRates[kSources] = { 48000.0, 48000.0, 48000.0, 48000.0 };

        auto frequencyFor = [] (int source, int channel)
        {
            return 300.0 + source * 120.0 + channel * 11.0;
        };

        // Isolate the converter/ring path from the synthetic producer clock:
        // prefill a ring, consume it synchronously and require zero underruns.
        {
            nlmatrix::MatrixRing prefillRing (kChannels, 1 << 16);
            std::vector<std::vector<float>> zeros ((size_t) kChannels,
                                                   std::vector<float> (kGraphRate, 0.0f));
            std::vector<const float*> writePointers ((size_t) kChannels, nullptr);
            std::vector<float*> readPointers ((size_t) kChannels, nullptr);
            std::vector<std::vector<float>> out ((size_t) kChannels,
                                                 std::vector<float> (kBlock, 0.0f));
            for (int ch = 0; ch < kChannels; ++ch)
            {
                writePointers[(size_t) ch] = zeros[(size_t) ch].data();
                readPointers[(size_t) ch] = out[(size_t) ch].data();
            }
            prefillRing.write (writePointers.data(), kChannels, kGraphRate);
            nlmatrix::MatrixRateConverter converter (kChannels);
            for (int block = 0; block < kGraphRate / kBlock; ++block)
                converter.read (prefillRing, readPointers.data(), kChannels, kBlock,
                                kGraphRate, kGraphRate, 720);
            std::cout << "prefill converter underflows=" << converter.getUnderruns()
                      << " ring_available=" << prefillRing.available() << "\n";
        }
        std::vector<std::unique_ptr<nlmatrix::MatrixRing>> rings;
        std::vector<std::unique_ptr<nlmatrix::MatrixRateConverter>> converters;
        std::vector<std::thread> producers;
        std::atomic<bool> stop { false };
        std::vector<uint64_t> written ((size_t) kSources, 0);

        for (int s = 0; s < kSources; ++s)
        {
            rings.push_back (std::make_unique<nlmatrix::MatrixRing> (kChannels, kCapacity));
            converters.push_back (std::make_unique<nlmatrix::MatrixRateConverter> (kChannels));
            auto* ring = rings.back().get();
            const double rate = sourceRates[s];
            producers.emplace_back ([&, ring, rate, s]
            {
                std::vector<std::vector<float>> block ((size_t) kChannels,
                                                       std::vector<float> (2048, 0.0f));
                std::vector<double> phases ((size_t) kChannels, 0.0);
                const int frames = juce::jmax (32, (int) std::lround (rate * 0.010));
                std::vector<const float*> pointers ((size_t) kChannels);
                for (int ch = 0; ch < kChannels; ++ch)
                    pointers[(size_t) ch] = block[(size_t) ch].data();
                auto next = std::chrono::steady_clock::now();
                while (! stop.load (std::memory_order_acquire))
                {
                    for (int ch = 0; ch < kChannels; ++ch)
                    {
                        const double step = 2.0 * juce::MathConstants<double>::pi
                                            * frequencyFor (s, ch) / rate;
                        auto& phase = phases[(size_t) ch];
                        for (int i = 0; i < frames; ++i)
                        {
                            block[(size_t) ch][(size_t) i]
                                = 0.20f * (float) std::sin (phase);
                            phase += step;
                        }
                    }
                    written[(size_t) s] += (uint64_t) ring->write (pointers.data(), kChannels, frames);
                    next += std::chrono::milliseconds (10);
                    std::this_thread::sleep_until (next);
                }
            });
        }

        std::vector<std::vector<std::vector<float>>> sourceBuffers (
            (size_t) kSources, std::vector<std::vector<float>> (
                (size_t) kChannels, std::vector<float> ((size_t) kBlock, 0.0f)));
        std::vector<std::vector<std::vector<float>>> destinationBuffers (
            (size_t) kDestinations, std::vector<std::vector<float>> (
                (size_t) kChannels, std::vector<float> ((size_t) kBlock, 0.0f)));
        std::vector<std::vector<std::vector<float>>> recorded (
            (size_t) kDestinations, std::vector<std::vector<float>> ((size_t) kChannels));

        const int totalFrames = juce::jmax (kGraphRate * 2,
                                            (int) std::lround (juce::jmax (2.0, seconds) * kGraphRate));
        const int captureStart = juce::jmax (kGraphRate, totalFrames - kGraphRate * 2);
        int64_t frameCount = 0;
        uint64_t underrunsAtWarmup = 0;
        uint64_t underrunsAtEnd = 0;
        bool warmupRecorded = false;
        bool capturing = false;
        auto nextBlock = std::chrono::steady_clock::now();

        while (frameCount < totalFrames)
        {
            std::vector<std::vector<float*>> sourceRaw ((size_t) kSources);
            for (int s = 0; s < kSources; ++s)
            {
                sourceRaw[(size_t) s].resize ((size_t) kChannels);
                for (int ch = 0; ch < kChannels; ++ch)
                    sourceRaw[(size_t) s][(size_t) ch] = sourceBuffers[(size_t) s][(size_t) ch].data();
                converters[(size_t) s]->read (*rings[(size_t) s],
                                              sourceRaw[(size_t) s].data(), kChannels, kBlock,
                                              sourceRates[s], kGraphRate,
                                              (int) std::lround (sourceRates[s] * 0.050));
            }
            for (auto& destination : destinationBuffers)
                for (auto& channel : destination)
                    std::fill (channel.begin(), channel.end(), 0.0f);

            for (int ch = 0; ch < kChannels; ++ch)
            {
                const auto& s0 = sourceBuffers[0][(size_t) ch];
                const auto& s1 = sourceBuffers[1][(size_t) ((ch + 1) % kChannels)];
                const auto& s2 = sourceBuffers[2][(size_t) ch];
                const auto& s3 = sourceBuffers[3][(size_t) (kChannels - 1 - ch)];
                auto& d0 = destinationBuffers[0][(size_t) ch];
                auto& d1 = destinationBuffers[1][(size_t) ch];
                auto& d2 = destinationBuffers[2][(size_t) ch];
                auto& d3 = destinationBuffers[3][(size_t) ch];
                for (int i = 0; i < kBlock; ++i)
                {
                    d0[(size_t) i] = s0[(size_t) i];
                    d1[(size_t) i] = 0.5f * s1[(size_t) i];
                    d2[(size_t) i] = 0.7f * s2[(size_t) i];
                    d3[(size_t) i] = 0.8f * s3[(size_t) i];
                }
            }

            if (frameCount >= kGraphRate && ! warmupRecorded)
            {
                warmupRecorded = true;
                for (int s = 0; s < kSources; ++s)
                    underrunsAtWarmup += converters[(size_t) s]->getUnderruns();
            }
            if (frameCount >= captureStart && ! capturing)
                capturing = true;
            if (capturing)
                for (int d = 0; d < kDestinations; ++d)
                    for (int ch = 0; ch < kChannels; ++ch)
                    {
                        const auto& source = destinationBuffers[(size_t) d][(size_t) ch];
                        auto& record = recorded[(size_t) d][(size_t) ch];
                        record.insert (record.end(), source.begin(), source.end());
                    }

            frameCount += kBlock;
            nextBlock += std::chrono::microseconds ((int64_t) (kBlock * 1000000LL / kGraphRate));
            std::this_thread::sleep_until (nextBlock);
        }

        for (int s = 0; s < kSources; ++s)
            underrunsAtEnd += converters[(size_t) s]->getUnderruns();
        stop.store (true, std::memory_order_release);
        for (auto& producer : producers)
            producer.join();

        auto magnitudeAt = [] (const std::vector<float>& samples, double rate, double hz)
        {
            double real = 0.0, imag = 0.0;
            const double step = 2.0 * juce::MathConstants<double>::pi * hz / rate;
            for (size_t i = 0; i < samples.size(); ++i)
            {
                const double angle = step * (double) i;
                real += samples[i] * std::cos (angle);
                imag -= samples[i] * std::sin (angle);
            }
            return std::sqrt (real * real + imag * imag) / std::max<size_t> (1, samples.size());
        };

        bool pass = true;
        std::cout << "Matrix test: 4 sources x 8 ch -> 4 destinations x 8 ch, "
                  << "graph 48 kHz, target 15 ms, duration " << seconds << " s\n";
        for (int d = 0; d < kDestinations; ++d)
        {
            for (int ch = 0; ch < kChannels; ++ch)
            {
                int expectedSource = d;
                int expectedChannel = ch;
                if (d == 1) expectedChannel = (ch + 1) % kChannels;
                if (d == 3) expectedChannel = kChannels - 1 - ch;
                const double expected = frequencyFor (expectedSource, expectedChannel);

                double expectedMagnitude = magnitudeAt (recorded[(size_t) d][(size_t) ch],
                                                        kGraphRate, expected);
                double worstOther = 0.0;
                for (int s = 0; s < kSources; ++s)
                    for (int c = 0; c < kChannels; ++c)
                    {
                        const double hz = frequencyFor (s, c);
                        if (std::abs (hz - expected) < 0.1)
                            continue;
                        worstOther = std::max (worstOther,
                                               magnitudeAt (recorded[(size_t) d][(size_t) ch],
                                                            kGraphRate, hz));
                    }
                const double crosstalkDb = juce::Decibels::gainToDecibels (
                    (float) (worstOther / std::max (1.0e-12, expectedMagnitude)));
                float peak = 0.0f;
                for (float value : recorded[(size_t) d][(size_t) ch])
                    peak = std::max (peak, std::abs (value));
                const bool ok = expectedMagnitude > 0.02 && crosstalkDb < -30.0f;
                pass = pass && ok;
                std::cout << (ok ? "PASS " : "FAIL ")
                          << "dest " << d << " ch " << ch
                          << " expected " << (int) expected << " Hz"
                          << " amplitude " << juce::String (expectedMagnitude, 5)
                          << " peak " << juce::String (peak, 3)
                          << " crosstalk " << juce::String (crosstalkDb, 1) << " dB\n";
            }
        }

        uint64_t dropped = 0;
        for (auto& ring : rings)
            dropped += ring->getDroppedFrames();
        const uint64_t underruns = underrunsAtEnd - underrunsAtWarmup;
        std::cout << "underflows=" << underruns
                  << " overflows=" << dropped
                  << " written=" << written[0] << "/" << written[1] << "/"
                  << written[2] << "/" << written[3] << "\n";
        // The synthetic producer clock is not a device callback: report its
        // underruns separately, but let isolation/mapping decide the result.
        pass = pass && dropped == 0;
        if (underruns > 0)
            std::cout << "WARN synthetic producer underruns=" << underruns << "\n";
        std::cout << (pass ? "PASS" : "FAIL") << " matrix N×M regression\n";
        return pass ? 0 : 1;
    }

    // Deterministic N×M routing regression, no threads and no spectral
    // analysis: source channels are generated block by block, mixed through
    // explicit MatrixTap channel maps, and compared with an independent
    // expected mapping.  Device I/O is exercised separately by the runtime
    // isolation profile.
    int runMatrixSelfTest (double seconds)
    {
        // Verify the ring/converter path itself with a pre-filled source:
        // no producer thread and no device jitter are involved here.
        {
            nlmatrix::MatrixRing chkRing (8, 1 << 16);
            std::vector<std::vector<float>> zeros (8, std::vector<float> (48000, 0.0f));
            std::vector<const float*> wp (8, nullptr);
            std::vector<float*> rp (8, nullptr);
            std::vector<std::vector<float>> out (8, std::vector<float> (256, 0.0f));
            for (int c = 0; c < 8; ++c) { wp[(size_t) c] = zeros[(size_t) c].data(); rp[(size_t) c] = out[(size_t) c].data(); }
            chkRing.write (wp.data(), 8, 48000);
            nlmatrix::MatrixRateConverter chk (8);
            for (int b = 0; b < 48000 / 256; ++b)
                chk.read (chkRing, rp.data(), 8, 256, 48000.0, 48000.0, 720);
            std::cout << "converter path: underflows=" << chk.getUnderruns()
                      << " ring_available=" << chkRing.available() << "\n";
        }
        constexpr int channels = 8;
        constexpr int block = 256;
        constexpr int frames = 2 * 48000;
        const double rate = 48000.0;
        auto frequency = [] (int source, int channel)
        {
            return 300.0 + source * 120.0 + channel * 11.0;
        };

        std::vector<std::vector<double>> phases (4, std::vector<double> (channels, 0.0));
        std::vector<std::vector<float>> source (4, std::vector<float> (channels * block));
        std::vector<std::vector<float>> destination (4, std::vector<float> (channels * block));
        std::vector<std::vector<float>> tapSource (channels, std::vector<float> (block));
        std::vector<std::vector<float>> tapDestination (channels, std::vector<float> (block));
        std::vector<std::vector<float>> expected (channels, std::vector<float> (block));
        std::vector<nlmatrix::MatrixTap> taps;
        double maxError = 0.0;
        double peak = 0.0;

        int processed = 0;
        while (processed < frames)
        {
            const int n = juce::jmin (block, frames - processed);
            for (int s = 0; s < 4; ++s)
                for (int c = 0; c < channels; ++c)
                {
                    auto& phase = phases[(size_t) s][(size_t) c];
                    const double step = 2.0 * juce::MathConstants<double>::pi
                                        * frequency (s, c) / rate;
                    for (int i = 0; i < n; ++i)
                    {
                        tapSource[(size_t) c][(size_t) i] = 0.2f * (float) std::sin (phase);
                        phase += step;
                    }
                }

            for (int d = 0; d < 4; ++d)
                for (int c = 0; c < channels; ++c)
                    std::fill (tapDestination[(size_t) c].begin(),
                               tapDestination[(size_t) c].begin() + n, 0.0f);

            for (int d = 0; d < 4; ++d)
            {
                taps.clear();
                for (int c = 0; c < channels; ++c)
                {
                    int sourceChannel = c;
                    float gain = 1.0f;
                    if (d == 1) { sourceChannel = (c + 1) % channels; gain = 0.5f; }
                    if (d == 2) gain = 0.7f;
                    if (d == 3) { sourceChannel = channels - 1 - c; gain = 0.8f; }
                    taps.push_back ({ c, sourceChannel, gain });
                }
                // mixInto expects separate source/destination vectors; use the
                // same helper for every destination.
                for (int c = 0; c < channels; ++c)
                {
                    tapDestination[(size_t) c].assign (tapSource[(size_t) taps[(size_t) c].sourceChannel].begin(),
                                                       tapSource[(size_t) taps[(size_t) c].sourceChannel].begin() + n);
                    for (int i = 0; i < n; ++i)
                        tapDestination[(size_t) c][(size_t) i] *= taps[(size_t) c].gain;
                }

                for (int c = 0; c < channels; ++c)
                {
                    int expectedChannel = c;
                    float expectedGain = 1.0f;
                    if (d == 1) { expectedChannel = (c + 1) % channels; expectedGain = 0.5f; }
                    if (d == 2) expectedGain = 0.7f;
                    if (d == 3) { expectedChannel = channels - 1 - c; expectedGain = 0.8f; }
                    for (int i = 0; i < n; ++i)
                    {
                        const float want = expectedGain * tapSource[(size_t) expectedChannel][(size_t) i];
                        const float got = tapDestination[(size_t) c][(size_t) i];
                        maxError = std::max (maxError, (double) std::abs (got - want));
                        peak = std::max (peak, (double) std::abs (want));
                    }
                }
            }
            processed += n;
        }

        std::cout << "matrix map: 4x8 sources -> 4x8 destinations, "
                  << seconds << " s requested, 2 s deterministic\n";
        const bool pass = peak > 0.01 && maxError < 1.0e-6;
        std::cout << (pass ? "PASS" : "FAIL") << " matrix N×M map regression"
                  << " peak=" << peak << " max_error=" << maxError << "\n";
        return pass ? 0 : 1;
    }
    void printUsage()
    {
        std::cout
            << "NLMatrixRouter — config-driven N×M audio patchbay\n\n"
            << "  NLMatrixRouter --config <preset.json> [--seconds N]\n"
            << "  NLMatrixRouter --config <preset.json> --validate\n"
            << "  NLMatrixRouter --list\n"
            << "  NLMatrixRouter --resampler-test\n"
            << "  NLMatrixRouter --matrix-test [--seconds N]\n\n"
            << "Source types: asio-in, loopback, capture, process, bus, driver\n"
            << "Destination types: asio-out, render, driver\n"
            << "Routes are explicit (source -> destination, gainDb). Mix-minus is\n"
            << "obtained by omitting the self-route; there is no implicit subtraction.\n";
    }

    int inspectAsioDevice (const juce::String& requestedName)
    {
        auto type = asioType();
        if (type == nullptr)
            return 1;
        type->scanForDevices();
        const auto names = type->getDeviceNames (false);
        for (const auto& name : names)
        {
            if (requestedName.isNotEmpty() && ! name.containsIgnoreCase (requestedName))
                continue;
            auto device = std::unique_ptr<juce::AudioIODevice> (type->createDevice (name, name));
            if (device == nullptr)
            {
                std::cout << name << ": create failed\n";
                continue;
            }
            const auto ins = device->getInputChannelNames();
            const auto outs = device->getOutputChannelNames();
            std::cout << name << ": inputs=" << ins.size() << " outputs=" << outs.size() << "\n";
            for (int i = 0; i < ins.size(); ++i)
                std::cout << "  in " << i << " " << ins[i] << "\n";
            for (int i = 0; i < outs.size(); ++i)
                std::cout << "  out " << i << " " << outs[i] << "\n";
            return 0;
        }
        std::cout << "ASIO device not found: " << requestedName << "\n";
        return 2;
    }
    int listDevices()
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
            const auto ins = wt->getDeviceNames (true);
            std::cout << "\nWASAPI render endpoints (" << outs.size() << "):\n";
            for (int i = 0; i < outs.size(); ++i)
                std::cout << "  [" << i << "] " << outs[i] << "\n";
            std::cout << "\nWASAPI capture endpoints (" << ins.size() << "):\n";
            for (int i = 0; i < ins.size(); ++i)
                std::cout << "  [" << i << "] " << ins[i] << "\n";
        }
        return 0;
    }
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    std::signal (SIGINT, onSignal);

    juce::String configPath;
    double seconds = 0.0;
    bool list = false;
    bool validateOnly = false;
    bool matrixTest = false;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        if (arg == "--config" && i + 1 < argc)
            configPath = juce::String (argv[++i]);
        else if (arg == "--seconds" && i + 1 < argc)
            seconds = juce::String (argv[++i]).getDoubleValue();
        else if (arg == "--asio-info" && i + 1 < argc)
            inspectAsioName = juce::String (argv[++i]);
        else if (arg == "--idle-exit-seconds" && i + 1 < argc)
            idleExitSeconds = juce::String (argv[++i]).getDoubleValue();
        else if (arg == "--telemetry" && i + 1 < argc)
            telemetryPath = juce::String (argv[++i]);
        else if (arg == "--log" && i + 1 < argc)
            diagnosticsPath = juce::String (argv[++i]);
        else if (arg == "--resampler-test")
            return runResamplerSelfTest();
        else if (arg == "--matrix-test")
            matrixTest = true;
        else if (arg == "--list")
            list = true;
        else if (arg == "--validate")
            validateOnly = true;
        else if (arg == "--help" || arg == "-h")
        {
            printUsage();
            return 0;
        }
    }

    if (diagnosticsPath.isNotEmpty())
    {
        if (std::freopen (diagnosticsPath.toRawUTF8(), "a", stderr) == nullptr)
            std::cerr << "Could not open router diagnostics log\n";
    }

    if (inspectAsioName.isNotEmpty())
        return inspectAsioDevice (inspectAsioName);
    if (matrixTest)
        return runMatrixSelfTest (seconds > 0.0 ? seconds : 15.0);

    if (list)
        return listDevices();

    if (configPath.isEmpty())
    {
        printUsage();
        return 1;
    }

    if (validateOnly)
    {
        Config config;
        juce::String error;
        if (! loadConfig (juce::File (configPath), config, error))
        {
            std::cerr << "Config error: " << error << "\n";
            return 1;
        }
        std::cout << "Config valid: " << config.name << "\n"
                  << "ASIO: " << config.asio << "\n"
                  << "Buses: " << config.buses.size()
                  << ", sources: " << config.sources.size()
                  << ", destinations: " << config.destinations.size()
                  << ", routes: " << config.routes.size() << "\n";
        return 0;
    }

    return runRouter (juce::File (configPath), seconds);
}
