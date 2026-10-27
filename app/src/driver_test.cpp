#ifndef NOMINMAX
    #define NOMINMAX
#endif

#include "nl_driver_client.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace
{
    void printInfo (const NLMATRIX_INFO& info)
    {
        std::cout << "NLMatrixAudio ABI " << info.Version
                  << " endpoints=" << info.EndpointCount
                  << " filters=" << info.RenderFilterCount << "/"
                  << info.CaptureFilterCount << "\n";
        for (unsigned long index = 0; index < info.EndpointCount
                                         && index < NLMATRIX_MAX_ENDPOINTS; ++index)
        {
            const auto& ep = info.Endpoints[index];
            std::cout << "  [" << index << "] render=" << ep.Render.SampleRate << " Hz/"
                      << ep.Render.Channels << " ch/"
                      << ep.Render.BitsPerSample << " bit"
                      << (ep.Render.IsFloat ? " float" : " int")
                      << " capture=" << ep.Capture.SampleRate << " Hz/"
                      << ep.Capture.Channels << " ch/"
                      << ep.Capture.BitsPerSample << " bit"
                      << (ep.Capture.IsFloat ? " float" : " int")
                      << " pushed=" << ep.RenderBytesPushed
                      << " pulled=" << ep.CaptureBytesPulled
                      << " peaks=" << ep.RenderPeak16 << "/" << ep.CapturePeak16
                      << " captureUsed=" << ep.CaptureUsedBytes
                      << " captureTarget=" << ep.CaptureTargetBytes
                      << " captureDropped=" << ep.CaptureDroppedBytes << "\n";
        }
    }

    // Exercises all published endpoint transports at once.  Each thread uses its
    // own file handle so this also covers concurrent user-mode access.
    int multiTone (const NLMATRIX_INFO& info, double seconds)
    {
        const unsigned long count = std::min<unsigned long> (info.EndpointCount,
                                                             NLMATRIX_MAX_ENDPOINTS);
        std::atomic<bool> stop { false };
        std::vector<std::thread> threads;
        std::vector<std::uint64_t> written ((size_t) count, 0);
        std::vector<std::atomic<int>> failed ((size_t) count);
        for (unsigned long i = 0; i < count; ++i)
            failed[i].store (0, std::memory_order_relaxed);

        for (unsigned long index = 0; index < count; ++index)
        {
            threads.emplace_back ([&, index]
            {
                NlDriverClient client;
                std::string error;
                if (! client.open (error))
                {
                    failed[index].store (1);
                    return;
                }
                const auto& fmt = info.Endpoints[index].Capture;
                const int channels = (int) std::max (1UL, fmt.Channels);
                const int bytesPerSample = (int) std::max (1UL, fmt.BitsPerSample / 8);
                const double rate = fmt.SampleRate > 0 ? (double) fmt.SampleRate : 48000.0;
                const int blockFrames = (int) std::max (64.0, std::floor (rate * 0.010));
                std::vector<std::uint8_t> block ((size_t) blockFrames * channels * bytesPerSample);
                double phase = 0.0;
                const double step = 2.0 * 3.14159265358979323846 * (440.0 + 110.0 * (double) index)
                                    / rate;
                while (! stop.load (std::memory_order_acquire))
                {
                    for (int frame = 0; frame < blockFrames; ++frame)
                    {
                        const float sample = (float) std::sin (phase) * 0.25f;
                        phase += step;
                        for (int ch = 0; ch < channels; ++ch)
                        {
                            auto* dst = block.data() + ((size_t) frame * channels + ch) * bytesPerSample;
                            if (fmt.IsFloat)
                                std::memcpy (dst, &sample, sizeof (float));
                            else if (fmt.BitsPerSample == 16)
                            {
                                const auto v = (int16_t) std::lround (sample * 32767.0f);
                                std::memcpy (dst, &v, sizeof (v));
                            }
                            else if (fmt.BitsPerSample == 24)
                            {
                                const auto v = (int32_t) std::lround (sample * 8388607.0f);
                                dst[0] = (uint8_t) (v & 0xff);
                                dst[1] = (uint8_t) ((v >> 8) & 0xff);
                                dst[2] = (uint8_t) ((v >> 16) & 0xff);
                            }
                        }
                    }
                    if (! client.writeCapture ((int) index, block.data(), (int) block.size(), error))
                    {
                        failed[index].store (1);
                        return;
                    }
                    written[index] += block.size();
                    std::this_thread::sleep_for (std::chrono::milliseconds (10));
                }
            });
        }

        std::this_thread::sleep_for (std::chrono::duration<double> (seconds));
        stop.store (true, std::memory_order_release);
        for (auto& t : threads)
            t.join();

        NLMATRIX_INFO after = {};
        std::string error;
        bool pass = true;
        NlDriverClient check;
        if (! check.open (error) || ! check.getInfo (after, error))
        {
            std::cerr << "Could not read counters after multitone: " << error << "\n";
            return 2;
        }
        for (unsigned long i = 0; i < count; ++i)
        {
            const auto delta = after.Endpoints[i].CaptureBytesPulled
                               - info.Endpoints[i].CaptureBytesPulled;
            const auto renderDelta = after.Endpoints[i].RenderBytesPushed
                                     - info.Endpoints[i].RenderBytesPushed;
            const bool ok = failed[i].load() == 0 && delta >= written[i] / 2 && renderDelta == 0;
            pass = pass && ok;
            std::cout << (ok ? "PASS " : "FAIL ")
                      << "endpoint " << i << " writes=" << written[i]
                      << " captureDelta=" << delta
                      << " renderDelta=" << renderDelta
                      << " failed=" << failed[i].load() << "\n";
        }
        return pass ? 0 : 1;
    }

    void writeTone (NlDriverClient& client, const NLMATRIX_INFO& info, int endpoint, double seconds)
    {
        const auto fmt = info.Endpoints[endpoint].Capture;
        const int channels = std::max (1UL, fmt.Channels);
        const int bytesPerSample = (int) std::max (1UL, fmt.BitsPerSample / 8);
        const int blockFrames = 480;
        std::vector<std::uint8_t> block ((size_t) blockFrames * channels * bytesPerSample);
        double phase = 0.0;
        const double step = 2.0 * 3.14159265358979323846 * 440.0 / (double) fmt.SampleRate;
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double> (seconds);

        while (std::chrono::steady_clock::now() < end)
        {
            for (int frame = 0; frame < blockFrames; ++frame)
            {
                const float sample = (float) (std::sin (phase) * 0.5);
                phase += step;
                for (int ch = 0; ch < channels; ++ch)
                {
                    auto* dst = block.data() + ((size_t) frame * channels + ch) * bytesPerSample;
                    if (fmt.IsFloat)
                        std::memcpy (dst, &sample, sizeof (float));
                    else if (fmt.BitsPerSample == 16)
                    {
                        const auto v = (int16_t) std::lround (sample * 32767.0f);
                        std::memcpy (dst, &v, sizeof (v));
                    }
                    else if (fmt.BitsPerSample == 24)
                    {
                        const auto v = (int32_t) std::lround (sample * 8388607.0f);
                        dst[0] = (uint8_t) (v & 0xff);
                        dst[1] = (uint8_t) ((v >> 8) & 0xff);
                        dst[2] = (uint8_t) ((v >> 16) & 0xff);
                    }
                    else
                    {
                        const auto v = (int32_t) std::lround (sample * 2147483647.0f);
                        std::memcpy (dst, &v, sizeof (v));
                    }
                }
            }

            std::string error;
            if (! client.writeCapture (endpoint, block.data(), (int) block.size(), error))
            {
                std::cerr << error << "\n";
                return;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }
    }

    void readRender (NlDriverClient& client, const NLMATRIX_INFO& info, int endpoint, double seconds)
    {
        std::vector<std::uint8_t> buffer (16384);
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double> (seconds);
        uint64_t total = 0;
        uint64_t frames = 0;
        uint64_t rising = 0;
        int peak = 0;
        float previous = 0.0f;
        while (std::chrono::steady_clock::now() < end)
        {
            std::string error;
            const int bytes = client.readRender (endpoint, buffer.data(), (int) buffer.size(), error);
            if (bytes < 0)
            {
                std::cerr << error << "\n";
                return;
            }
            total += (uint64_t) std::max (0, bytes);
            const auto& fmt = info.Endpoints[endpoint].Render;
            if (fmt.BitsPerSample == 16)
            {
                const int blockAlign = std::max (1UL, fmt.BlockAlign);
                const int channels = std::max (1UL, fmt.Channels);
                const auto* p = reinterpret_cast<const int16_t*>(buffer.data());
                const int sampleFrames = bytes / blockAlign;
                for (int i = 0; i < sampleFrames; ++i)
                {
                    const int value = p[(size_t) i * channels];
                    const float f = (float) value / 32768.0f;
                    peak = std::max (peak, std::abs (value));
                    if (previous <= 0.0f && f > 0.0f) ++rising;
                    previous = f;
                    ++frames;
                }
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
        const double secondsRecorded = frames / (double) std::max (1UL, info.Endpoints[endpoint].Render.SampleRate);
        const double frequency = secondsRecorded > 0.0 ? rising / secondsRecorded : 0.0;
        std::cout << "Read " << total << " render bytes, peak16=" << peak
                  << " freq=" << frequency << " Hz\n";
    }

    void loopback (NlDriverClient& client, int endpoint, double seconds)
    {
        std::vector<std::uint8_t> buffer (16384);
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double> (seconds);
        uint64_t total = 0;
        while (std::chrono::steady_clock::now() < end)
        {
            std::string error;
            const int bytes = client.readRender (endpoint, buffer.data(), (int) buffer.size(), error);
            if (bytes < 0)
            {
                std::cerr << error << "\n";
                return;
            }
            if (bytes > 0)
            {
                if (! client.writeCapture (endpoint, buffer.data(), bytes, error))
                {
                    std::cerr << error << "\n";
                    return;
                }
                total += (uint64_t) bytes;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
        std::cout << "Forwarded " << total << " bytes.\n";
    }
}

int main (int argc, char** argv)
{
    NlDriverClient client;
    std::string error;
    if (! client.open (error))
    {
        std::cerr << error << "\nInstall the NLMatrixAudio driver first.\n";
        return 1;
    }

    NLMATRIX_INFO info = {};
    if (! client.getInfo (info, error))
    {
        std::cerr << error << "\n";
        return 2;
    }
    printInfo (info);

    double seconds = 5.0;
    int endpoint = 0;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--endpoint") == 0 && i + 1 < argc)
        {
            endpoint = std::max (0, std::atoi (argv[++i]));
            if ((unsigned long) endpoint >= info.EndpointCount)
            {
                std::cerr << "Endpoint " << endpoint << " out of range ("
                          << info.EndpointCount << " endpoints)\n";
                return 6;
            }
        }
        if (std::strcmp (argv[i], "--tone") == 0)
        {
            writeTone (client, info, endpoint, seconds);
            return 0;
        }
        if (std::strcmp (argv[i], "--multitone") == 0)
            return multiTone (info, seconds);
        if (std::strcmp (argv[i], "--loopback") == 0)
        {
            loopback (client, endpoint, seconds);
            return 0;
        }
        if (std::strcmp (argv[i], "--read") == 0)
        {
            readRender (client, info, endpoint, seconds);
            return 0;
        }
        if (std::strcmp (argv[i], "--reset") == 0)
        {
            if (! client.reset (error))
            {
                std::cerr << error << "\n";
                return 3;
            }
            std::cout << "Reset.\n";
            return 0;
        }
    }

    std::cout << "Usage: NLMatrixDriverTest [--endpoint N] [--tone|--read|--loopback|--reset|--multitone]\n";
    return 0;
}
