#include "wasapi_loopback.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    struct Options
    {
        std::wstring endpoint;
        std::string outPath;
        double seconds = 5.0;
    };

    void writeWav (const std::string& path, const std::vector<float>& samples, int sampleRate)
    {
        std::ofstream out (path, std::ios::binary);
        if (! out) return;
        const uint32_t dataBytes = (uint32_t) (samples.size() * 2);
        const uint32_t riffSize = 36 + dataBytes;
        const uint16_t channels = 1, bits = 16;
        const uint32_t rate = (uint32_t) sampleRate;
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
        for (float sample : samples)
        {
            const auto value = (int16_t) std::lround (std::clamp (sample, -1.0f, 1.0f) * 32767.0f);
            out.write (reinterpret_cast<const char*> (&value), 2);
        }
    }
}

int main (int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a (argv[i]);
        if (a == "--endpoint" && i + 1 < argc) { const char* v = argv[++i]; o.endpoint.assign (v, v + std::strlen (v)); }
        else if (a == "--out" && i + 1 < argc) o.outPath = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) o.seconds = std::atof (argv[++i]);
    }
    if (o.endpoint.empty() || o.outPath.empty() || o.seconds <= 0.0)
    {
        std::cout << "Usage: NLMatrixLoopbackTest --endpoint \"<render endpoint>\" --out file.wav [--seconds N]\n";
        return 1;
    }

    WasapiLoopbackCapture capture;
    std::mutex lock;
    std::vector<float> samples;
    std::atomic<uint64_t> frames { 0 };
    std::atomic<double> energy { 0.0 };
    std::atomic<float> peak { 0.0f };
    std::string error;
    if (! capture.start (o.endpoint, [&] (const float* left, const float*, int n)
        {
            if (left == nullptr || n <= 0) return;
            std::lock_guard<std::mutex> guard (lock);
            if (samples.size() < (size_t) (capture.getSampleRate() * 10.0))
                samples.insert (samples.end(), left, left + n);
            double sum = 0.0;
            for (int i = 0; i < n; ++i)
            {
                sum += (double) left[i] * left[i];
                peak.store (std::max (peak.load(), std::abs (left[i])));
            }
            energy.fetch_add (sum);
            frames.fetch_add ((uint64_t) n);
        }, error))
    {
        std::cerr << "Loopback start failed: " << error << "\n";
        return 2;
    }

    std::this_thread::sleep_for (std::chrono::duration<double> (o.seconds));
    capture.stop();
    std::lock_guard<std::mutex> guard (lock);
    const int sr = (int) capture.getSampleRate();
    const double rms = frames.load() > 0 ? std::sqrt (energy.load() / (double) frames.load()) : 0.0;
    writeWav (o.outPath, samples, sr > 0 ? sr : 48000);
    std::cout << "Loopback endpoint=" << std::string (o.endpoint.begin(), o.endpoint.end())
              << " frames=" << frames.load() << " sr=" << sr
              << " rms=" << rms << " peak=" << peak.load()
              << " out=" << o.outPath << "\n";
    return 0;
}
