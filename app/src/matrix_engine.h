#pragma once

// Generic N-channel building blocks used by the multichannel matrix
// diagnostics (and intended to replace the stereo-only runtime next).
// Independent implementation: plain SPSC ring, streaming fractional converter
// and a deterministic mix helper. No third-party or AGPL code.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef max
#undef max
#undef min
#endif

namespace nlmatrix
{
    class MatrixRing
    {
    public:
        MatrixRing (int numChannels, int capacityFrames)
            : channels (std::max (1, numChannels)),
              capacity (capacityFrames),
              data ((size_t) channels * (size_t) capacity, 0.0f)
        {
            assert ((capacity & (capacity - 1)) == 0);
        }

        int getChannels() const noexcept { return channels; }
        int getCapacity() const noexcept { return capacity; }
        int available() const noexcept
        {
            return (int) (writePos.load (std::memory_order_acquire)
                          - readPos.load (std::memory_order_acquire));
        }
        uint64_t getDroppedFrames() const noexcept { return droppedFrames.load(); }

        int write (const float* const* source, int numSourceChannels, int numFrames) noexcept
        {
            const auto w = writePos.load (std::memory_order_relaxed);
            const auto r = readPos.load (std::memory_order_acquire);
            const int freeFrames = capacity - (int) (w - r);
            const int toWrite = std::min (numFrames, freeFrames);
            const int copyChannels = std::min (numSourceChannels, channels);
            for (int i = 0; i < toWrite; ++i)
            {
                const auto index = (size_t) ((w + (uint64_t) i) & (uint64_t) (capacity - 1));
                auto* frame = data.data() + index * (size_t) channels;
                for (int ch = 0; ch < channels; ++ch)
                    frame[ch] = ch < copyChannels && source[ch] != nullptr
                                    ? source[ch][i] : 0.0f;
            }
            writePos.store (w + (uint64_t) toWrite, std::memory_order_release);
            if (toWrite < numFrames)
                droppedFrames.fetch_add ((uint64_t) (numFrames - toWrite));
            return toWrite;
        }

        int read (float* const* destination, int numDestinationChannels, int numFrames) noexcept
        {
            const auto r = readPos.load (std::memory_order_relaxed);
            const auto w = writePos.load (std::memory_order_acquire);
            const int toRead = std::min (numFrames, (int) (w - r));
            const int copyChannels = std::min (numDestinationChannels, channels);
            for (int i = 0; i < toRead; ++i)
            {
                const auto index = (size_t) ((r + (uint64_t) i) & (uint64_t) (capacity - 1));
                const auto* frame = data.data() + index * (size_t) channels;
                for (int ch = 0; ch < numDestinationChannels; ++ch)
                    if (destination[ch] != nullptr)
                        destination[ch][i] = ch < copyChannels ? frame[ch] : 0.0f;
            }
            readPos.store (r + (uint64_t) toRead, std::memory_order_release);
            return toRead;
        }

        bool readFrame (float* destination, int numDestinationChannels) noexcept
        {
            const auto r = readPos.load (std::memory_order_relaxed);
            const auto w = writePos.load (std::memory_order_acquire);
            if (r == w)
                return false;
            const auto index = (size_t) (r & (uint64_t) (capacity - 1));
            const auto* frame = data.data() + index * (size_t) channels;
            const int copyChannels = std::min (numDestinationChannels, channels);
            for (int ch = 0; ch < numDestinationChannels; ++ch)
                destination[ch] = ch < copyChannels ? frame[ch] : 0.0f;
            readPos.store (r + 1, std::memory_order_release);
            return true;
        }

    private:
        int channels;
        int capacity;
        std::vector<float> data;
        std::atomic<uint64_t> writePos { 0 };
        std::atomic<uint64_t> readPos { 0 };
        std::atomic<uint64_t> droppedFrames { 0 };
    };

    // N-channel streaming fractional-rate converter. One shared phase and one
    // fade/previous/current state per channel.
    class MatrixRateConverter
    {
    public:
        explicit MatrixRateConverter (int numChannels = 0)
        {
            reset (numChannels);
        }

        static constexpr double MAX_TRIM = 300.0e-6;

        void reset (int numChannels)
        {
            channels = std::max (0, numChannels);
            phase = 0.0;
            trim = 0.0;
            haveFrame = false;
            underruns = 0;
            previous.assign ((size_t) channels, 0.0f);
            current.assign ((size_t) channels, 0.0f);
            fade.assign ((size_t) channels, 0.0f);
            scratch.assign ((size_t) channels, 0.0f);
        }

        int getChannels() const noexcept { return channels; }
        uint64_t getUnderruns() const noexcept { return underruns; }
        double getTrim() const noexcept { return trim; }

        void read (MatrixRing& ring, float* const* destination, int destinationChannels,
                   int numFrames, double sourceRate, double destinationRate,
                   int targetFrames) noexcept
        {
            const double baseRatio = sourceRate / destinationRate;
            if (! (baseRatio > 0.0) || numFrames <= 0 || channels <= 0)
            {
                for (int ch = 0; ch < destinationChannels; ++ch)
                    if (destination[ch] != nullptr)
                        std::fill (destination[ch], destination[ch] + std::max (0, numFrames), 0.0f);
                return;
            }

            if (targetFrames > 0)
            {
                const double error = (double) (ring.available() - targetFrames)
                                     / (double) targetFrames;
                const double correction = std::clamp (error * 0.0005, -MAX_TRIM, MAX_TRIM);
                trim = trim * 0.995 + correction * 0.005;
            }
            const double ratio = baseRatio * (1.0 + trim);

            if (! haveFrame)
            {
                if (! ring.readFrame (scratch.data(), channels))
                {
                    for (int ch = 0; ch < destinationChannels; ++ch)
                        if (destination[ch] != nullptr)
                            std::fill (destination[ch], destination[ch] + numFrames, 0.0f);
                    return;
                }
                for (int ch = 0; ch < channels; ++ch)
                {
                    previous[(size_t) ch] = scratch[(size_t) ch];
                    current[(size_t) ch] = scratch[(size_t) ch];
                    fade[(size_t) ch] = 0.0f;
                }
                haveFrame = true;
                phase = 0.0;
            }

            for (int i = 0; i < numFrames; ++i)
            {
                bool underrun = false;
                while (phase >= 1.0)
                {
                    for (int ch = 0; ch < channels; ++ch)
                        previous[(size_t) ch] = current[(size_t) ch];
                    if (! ring.readFrame (current.data(), channels))
                    {
                        underrun = true;
                        break;
                    }
                    phase -= 1.0;
                }

                if (underrun)
                {
                    ++underruns;
                    for (int ch = 0; ch < destinationChannels; ++ch)
                        if (destination[ch] != nullptr)
                            std::fill (destination[ch] + i, destination[ch] + numFrames, 0.0f);
                    std::fill (previous.begin(), previous.end(), 0.0f);
                    std::fill (current.begin(), current.end(), 0.0f);
                    std::fill (fade.begin(), fade.end(), 0.0f);
                    haveFrame = false;
                    phase = 0.0;
                    return;
                }

                const float t = (float) std::clamp (phase, 0.0, 1.0);
                for (int ch = 0; ch < destinationChannels; ++ch)
                {
                    if (destination[ch] == nullptr)
                        continue;
                    const int sourceChannel = ch < channels ? ch : 0;
                    const float sample = previous[(size_t) sourceChannel]
                                         + (current[(size_t) sourceChannel]
                                            - previous[(size_t) sourceChannel]) * t;
                    destination[ch][i] = sample * fade[(size_t) sourceChannel];
                    if (ch < channels)
                        fade[(size_t) ch] = std::min (1.0f, fade[(size_t) ch] + 1.0f / 64.0f);
                }
                phase += ratio;
            }
        }

    private:
        int channels = 0;
        double phase = 0.0;
        double trim = 0.0;
        bool haveFrame = false;
        uint64_t underruns = 0;
        std::vector<float> previous, current, fade, scratch;
    };

    // One matrix contribution: destination channel <- source channel with a
    // gain.  A destination can have any number of contributions.
    struct MatrixTap
    {
        int destinationChannel = 0;
        int sourceChannel = 0;
        float gain = 1.0f;
    };

    inline void mixInto (std::vector<std::vector<float>>& destination,
                         const std::vector<std::vector<float>>& source,
                         const std::vector<MatrixTap>& taps, int numFrames)
    {
        for (const auto& tap : taps)
        {
            if (tap.destinationChannel < 0
                || tap.destinationChannel >= (int) destination.size()
                || tap.sourceChannel < 0
                || tap.sourceChannel >= (int) source.size())
                continue;
            auto& dst = destination[(size_t) tap.destinationChannel];
            const auto& src = source[(size_t) tap.sourceChannel];
            const int count = std::min (numFrames, (int) std::min (dst.size(), src.size()));
            for (int i = 0; i < count; ++i)
                dst[(size_t) i] += src[(size_t) i] * tap.gain;
        }
    }
}
