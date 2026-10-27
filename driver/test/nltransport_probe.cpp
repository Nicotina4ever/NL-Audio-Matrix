// nltransport_probe.cpp - minimal ABI v3 client for the NLMatrix virtual audio
// transport.  Used to verify the driver after a VM reinstall and to exercise
// the capture/render counters without the (separately rebuilt) router.
//
// Modes:
//   (no args) or --info          print NLMATRIX_INFO (ABI v3 counters included)
//   --version                    print info, then require Version==3/EndpointCount==4
//   --target EP MS               NLMATRIX_IOCTL_SET_CAPTURE_TARGET, then print endpoint
//   --write EP SECONDS [--stall MS] [--chunk-ms MS] [--tone-hz HZ] [--amp A]
//                                write a tone into the router->capture ring at a
//                                constant cadence (simulates the user-mode router)
//   --read EP SECONDS            drain the render ring (simulates the router)
//   --monitor EP SECONDS         poll GET_INFO every 250 ms while something else runs
//   --reset                      NLMATRIX_IOCTL_RESET (all endpoints)
//
// Build with driver/build-probe.ps1 (MSVC x64, host side).

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "nlmatrix_ioctl.h"

#pragma comment(lib, "winmm.lib")

// ABI v3 layout contract (writer + driver + probe must agree).  Pack(8) with
// 32-bit fields => 104-byte endpoint info, 436-byte info blob.
static_assert (sizeof (NLMATRIX_ENDPOINT_INFO) == 104, "ABI v3 NLMATRIX_ENDPOINT_INFO layout changed");
static_assert (sizeof (NLMATRIX_INFO) == 436, "ABI v3 NLMATRIX_INFO layout changed");
static_assert (offsetof (NLMATRIX_ENDPOINT_INFO, Active) == 80, "ABI v3 Active offset");
static_assert (offsetof (NLMATRIX_ENDPOINT_INFO, CaptureTargetMs) == 84, "ABI v3 CaptureTargetMs offset");
static_assert (offsetof (NLMATRIX_ENDPOINT_INFO, CaptureWriteCalls) == 88, "ABI v3 CaptureWriteCalls offset");
static_assert (offsetof (NLMATRIX_ENDPOINT_INFO, CaptureUnderrunBytes) == 92, "ABI v3 CaptureUnderrunBytes offset");
static_assert (offsetof (NLMATRIX_ENDPOINT_INFO, CaptureUnderrunCalls) == 96, "ABI v3 CaptureUnderrunCalls offset");
static_assert (offsetof (NLMATRIX_ENDPOINT_INFO, RenderOverrunBytes) == 100, "ABI v3 RenderOverrunBytes offset");
static_assert (sizeof (NLMATRIX_CAPTURE_TARGET_REQUEST) == 8, "ABI v3 SET_CAPTURE_TARGET request size");

namespace
{
HANDLE g_device = INVALID_HANDLE_VALUE;

bool openDevice()
{
    g_device = CreateFileW (NLMATRIX_USER_PATH, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    if (g_device == INVALID_HANDLE_VALUE)
    {
        std::printf ("ERROR CreateFile(%ls) failed Win32=%lu\n", NLMATRIX_USER_PATH, GetLastError());
        return false;
    }
    return true;
}

void closeDevice()
{
    if (g_device != INVALID_HANDLE_VALUE)
    {
        CloseHandle (g_device);
        g_device = INVALID_HANDLE_VALUE;
    }
}

bool getInfo (NLMATRIX_INFO& info)
{
    std::memset (&info, 0, sizeof (info));
    DWORD bytes = 0;
    if (! DeviceIoControl (g_device, NLMATRIX_IOCTL_GET_INFO, nullptr, 0,
                           &info, static_cast<DWORD> (sizeof (info)), &bytes, nullptr))
    {
        std::printf ("ERROR GET_INFO failed Win32=%lu\n", GetLastError());
        return false;
    }
    if (bytes < sizeof (NLMATRIX_INFO))
    {
        std::printf ("ERROR GET_INFO returned %lu bytes, expected %llu\n",
                     (unsigned long) bytes, (unsigned long long) sizeof (NLMATRIX_INFO));
        return false;
    }
    return true;
}

void printStream (const char* label, const NLMATRIX_STREAM_INFO& s)
{
    std::printf ("%s=%lu Hz/%lu ch/%lu bit %s align=%lu", label,
                 s.SampleRate, s.Channels, s.BitsPerSample,
                 s.IsFloat ? "float" : "int", s.BlockAlign);
}

void printEndpoint (unsigned long index, const NLMATRIX_ENDPOINT_INFO& e)
{
    std::printf ("[%lu] Active=%lu ", index, e.Active);
    printStream ("render", e.Render);
    std::printf (" ");
    printStream ("capture", e.Capture);
    std::printf ("\n");
    std::printf ("    render : capBytes=%lu pushed=%lu peak16=%lu overrunBytes=%lu\n",
                 e.RenderCapacityBytes, e.RenderBytesPushed, e.RenderPeak16,
                 e.RenderOverrunBytes);
    std::printf ("    capture: capBytes=%lu pulled=%lu peak16=%lu used=%lu targetBytes=%lu "
                 "targetMs=%lu dropped=%lu writeCalls=%lu\n",
                 e.CaptureCapacityBytes, e.CaptureBytesPulled, e.CapturePeak16,
                 e.CaptureUsedBytes, e.CaptureTargetBytes, e.CaptureTargetMs,
                 e.CaptureDroppedBytes, e.CaptureWriteCalls);
    std::printf ("    counters: underrunBytes=%lu underrunCalls=%lu\n",
                 e.CaptureUnderrunBytes, e.CaptureUnderrunCalls);
}

void printInfo (const NLMATRIX_INFO& info)
{
    std::printf ("NLMatrixAudio ABI %lu endpoints=%lu filters=%lu/%lu infoBytes=%lu endpointBytes=%lu\n",
                 info.Version, info.EndpointCount, info.RenderFilterCount,
                 info.CaptureFilterCount, info.Size, info.Endpoints[0].Size);
    const unsigned long count = std::min<unsigned long> (info.EndpointCount, NLMATRIX_MAX_ENDPOINTS);
    for (unsigned long i = 0; i < count; ++i)
        printEndpoint (i, info.Endpoints[i]);
}

bool setCaptureTarget (unsigned long endpoint, unsigned long targetMs)
{
    NLMATRIX_CAPTURE_TARGET_REQUEST request {};
    request.EndpointIndex = endpoint;
    request.CaptureTargetMs = targetMs;
    DWORD bytes = 0;
    if (! DeviceIoControl (g_device, NLMATRIX_IOCTL_SET_CAPTURE_TARGET,
                           &request, sizeof (request), nullptr, 0, &bytes, nullptr))
    {
        std::printf ("ERROR SET_CAPTURE_TARGET ep=%lu ms=%lu failed Win32=%lu\n",
                     endpoint, targetMs, GetLastError());
        return false;
    }
    return true;
}

int writeCapture (unsigned long endpoint, const void* data, unsigned long bytes)
{
    std::vector<unsigned char> packet (sizeof (NLMATRIX_WRITE_REQUEST) + bytes);
    auto* request = reinterpret_cast<NLMATRIX_WRITE_REQUEST*> (packet.data());
    request->EndpointIndex = endpoint;
    request->DataBytes = bytes;
    std::memcpy (packet.data() + sizeof (NLMATRIX_WRITE_REQUEST), data, bytes);
    DWORD written = 0;
    if (! DeviceIoControl (g_device, NLMATRIX_IOCTL_WRITE_CAPTURE,
                           packet.data(), static_cast<DWORD> (packet.size()),
                           nullptr, 0, &written, nullptr))
    {
        std::printf ("ERROR WRITE_CAPTURE ep=%lu bytes=%lu failed Win32=%lu\n",
                     endpoint, bytes, GetLastError());
        return -1;
    }
    return (int) written;
}

int readRender (unsigned long endpoint, void* destination, unsigned long maxBytes)
{
    NLMATRIX_ENDPOINT_REQUEST request {};
    request.EndpointIndex = endpoint;
    DWORD bytes = 0;
    if (! DeviceIoControl (g_device, NLMATRIX_IOCTL_READ_RENDER, &request, sizeof (request),
                           destination, maxBytes, &bytes, nullptr))
    {
        std::printf ("ERROR READ_RENDER ep=%lu failed Win32=%lu\n", endpoint, GetLastError());
        return -1;
    }
    return (int) bytes;
}

bool resetTransport()
{
    DWORD bytes = 0;
    if (! DeviceIoControl (g_device, NLMATRIX_IOCTL_RESET, nullptr, 0, nullptr, 0, &bytes, nullptr))
    {
        std::printf ("ERROR RESET failed Win32=%lu\n", GetLastError());
        return false;
    }
    return true;
}

struct ToneFormat
{
    unsigned long sampleRate = 48000;
    unsigned long channels = 1;
    unsigned long blockAlign = 2;
    unsigned long bitsPerSample = 16;
    bool isFloat = false;
};

bool toneFormatFromInfo (const NLMATRIX_INFO& info, unsigned long endpoint, ToneFormat& format)
{
    const auto& stream = info.Endpoints[std::min<unsigned long> (endpoint, NLMATRIX_MAX_ENDPOINTS - 1)].Capture;
    if (stream.SampleRate == 0 || stream.BlockAlign == 0 || stream.Channels == 0)
    {
        std::printf ("ERROR endpoint %lu has no live capture format yet (open/record the capture endpoint first)\n", endpoint);
        return false;
    }
    format.sampleRate = stream.SampleRate;
    format.channels = stream.Channels;
    format.blockAlign = stream.BlockAlign;
    format.bitsPerSample = stream.BitsPerSample;
    format.isFloat = stream.IsFloat != 0;
    if (! ((format.bitsPerSample == 16 && ! format.isFloat) || (format.bitsPerSample == 32 && format.isFloat)))
    {
        std::printf ("ERROR unsupported capture format for probe: %lu bit %s\n",
                     format.bitsPerSample, format.isFloat ? "float" : "int");
        return false;
    }
    return true;
}

void fillTone (std::vector<unsigned char>& buffer, const ToneFormat& format,
               double frequency, double amplitude, std::vector<double>& phases)
{
    const unsigned long frames = static_cast<unsigned long> (buffer.size() / format.blockAlign);
    const double twoPi = 6.28318530717958647692;
    unsigned char* out = buffer.data();
    for (unsigned long frame = 0; frame < frames; ++frame)
    {
        for (unsigned long channel = 0; channel < format.channels; ++channel)
        {
            const double channelFrequency = format.channels > 1 ? frequency * (channel + 1) : frequency;
            const double value = amplitude * std::sin (phases[channel]);
            phases[channel] += twoPi * channelFrequency / static_cast<double> (format.sampleRate);
            if (phases[channel] > twoPi)
                phases[channel] -= twoPi;
            if (format.isFloat)
            {
                const float sample = static_cast<float> (value);
                std::memcpy (out, &sample, sizeof (sample));
                out += sizeof (sample);
            }
            else
            {
                const long clamped = std::lround (value * 32767.0);
                const int16_t sample = static_cast<int16_t> (std::max (-32768L, std::min (32767L, clamped)));
                std::memcpy (out, &sample, sizeof (sample));
                out += sizeof (sample);
            }
        }
    }
}

double nowMs (LARGE_INTEGER start, LARGE_INTEGER frequency)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter (&now);
    return (now.QuadPart - start.QuadPart) * 1000.0 / static_cast<double> (frequency.QuadPart);
}

void sleepUntil (double dueMs, LARGE_INTEGER start, LARGE_INTEGER frequency)
{
    for (;;)
    {
        const double remaining = dueMs - nowMs (start, frequency);
        if (remaining <= 0.0)
            return;
        if (remaining >= 1.5)
            Sleep (static_cast<DWORD> (remaining - 1.0));
        else
            std::this_thread::yield();
    }
}

int runWrite (unsigned long endpoint, double seconds, double stallMs, double chunkMs,
              double toneHz, double amplitude)
{
    NLMATRIX_INFO info {};
    if (! getInfo (info))
        return 1;
    ToneFormat format;
    if (! toneFormatFromInfo (info, endpoint, format))
        return 1;

    const unsigned long frames = std::max<unsigned long> (
        1, static_cast<unsigned long> (format.sampleRate * chunkMs / 1000.0));
    const unsigned long chunkBytes = frames * format.blockAlign;
    std::vector<unsigned char> chunk (chunkBytes);
    std::vector<double> phases (format.channels, 0.0);
    fillTone (chunk, format, toneHz, amplitude, phases);

    std::printf ("WRITE ep=%lu %.2fs chunk=%.1fms tone=%.1fHz amp=%.2f format=%luHz/%luch/%lubit->%lu bytes/chunk\n",
                 endpoint, seconds, chunkMs, toneHz, amplitude,
                 format.sampleRate, format.channels, format.bitsPerSample, chunkBytes);

    LARGE_INTEGER frequency, start;
    QueryPerformanceFrequency (&frequency);
    QueryPerformanceCounter (&start);
    timeBeginPeriod (1);

    unsigned long chunks = 0;
    unsigned long long bytesWritten = 0;
    bool stalled = false;
    const double stallAtMs = seconds * 1000.0 / 2.0;
    int failures = 0;
    for (;;)
    {
        const double dueMs = static_cast<double> (chunks) * chunkMs;
        if (dueMs >= seconds * 1000.0)
            break;
        if (stallMs > 0.0 && ! stalled && nowMs (start, frequency) >= stallAtMs)
        {
            std::printf ("STALL sleeping %.0f ms at t=%.0f ms (simulating a router scheduling stall)\n",
                         stallMs, nowMs (start, frequency));
            Sleep (static_cast<DWORD> (stallMs));
            stalled = true;
        }
        sleepUntil (dueMs, start, frequency);
        const int written = writeCapture (endpoint, chunk.data(), chunkBytes);
        if (written < 0)
        {
            if (++failures > 3)
                break;
            continue;
        }
        bytesWritten += static_cast<unsigned long long> (written);
        ++chunks;
        if ((chunks % 100) == 0)
            std::printf ("  t=%.1fs chunks=%lu bytes=%llu\n",
                         nowMs (start, frequency) / 1000.0, chunks, bytesWritten);
    }

    timeEndPeriod (1);
    NLMATRIX_INFO after {};
    if (! getInfo (after))
        return 1;
    std::printf ("WRITE-DONE chunks=%lu bytes=%llu\n", chunks, bytesWritten);
    printEndpoint (endpoint, after.Endpoints[std::min<unsigned long> (endpoint, NLMATRIX_MAX_ENDPOINTS - 1)]);
    std::printf ("RESULT write ep=%lu chunks=%lu bytes=%llu writeCalls=%lu underrunBytes=%lu underrunCalls=%lu dropped=%lu\n",
                 endpoint, chunks, bytesWritten,
                 after.Endpoints[endpoint].CaptureWriteCalls,
                 after.Endpoints[endpoint].CaptureUnderrunBytes,
                 after.Endpoints[endpoint].CaptureUnderrunCalls,
                 after.Endpoints[endpoint].CaptureDroppedBytes);
    return 0;
}

int runRead (unsigned long endpoint, double seconds)
{
    NLMATRIX_INFO info {};
    if (! getInfo (info))
        return 1;
    const auto& render = info.Endpoints[std::min<unsigned long> (endpoint, NLMATRIX_MAX_ENDPOINTS - 1)].Render;
    const unsigned long chunkFrames = std::max<unsigned long> (
        1, static_cast<unsigned long> (render.SampleRate * 10 / 1000));
    const unsigned long chunkBytes = std::max<unsigned long> (4, chunkFrames * std::max<unsigned long> (1, render.BlockAlign));
    std::vector<unsigned char> chunk (chunkBytes);
    LARGE_INTEGER frequency, start;
    QueryPerformanceFrequency (&frequency);
    QueryPerformanceCounter (&start);
    timeBeginPeriod (1);
    unsigned long long bytesRead = 0;
    unsigned long calls = 0;
    double nextMs = 0.0;
    while (nowMs (start, frequency) < seconds * 1000.0)
    {
        const int got = readRender (endpoint, chunk.data(), chunkBytes);
        if (got < 0)
            break;
        bytesRead += static_cast<unsigned long long> (got);
        ++calls;
        nextMs += 10.0;
        sleepUntil (nextMs, start, frequency);
    }
    timeEndPeriod (1);
    NLMATRIX_INFO after {};
    getInfo (after);
    std::printf ("READ-DONE ep=%lu calls=%lu bytes=%llu\n", endpoint, calls, bytesRead);
    printEndpoint (endpoint, after.Endpoints[std::min<unsigned long> (endpoint, NLMATRIX_MAX_ENDPOINTS - 1)]);
    return 0;
}

int runMonitor (unsigned long endpoint, double seconds)
{
    LARGE_INTEGER frequency, start;
    QueryPerformanceFrequency (&frequency);
    QueryPerformanceCounter (&start);
    std::printf ("MONITOR ep=%lu %.1fs\n", endpoint, seconds);
    while (nowMs (start, frequency) < seconds * 1000.0)
    {
        NLMATRIX_INFO info {};
        if (! getInfo (info))
            return 1;
        const auto& e = info.Endpoints[std::min<unsigned long> (endpoint, NLMATRIX_MAX_ENDPOINTS - 1)];
        std::printf ("t=%.1f used=%lu target=%lu(%lums) pulled=%lu underrB=%lu underrC=%lu dropped=%lu writes=%lu pushed=%lu overrB=%lu\n",
                     nowMs (start, frequency) / 1000.0,
                     e.CaptureUsedBytes, e.CaptureTargetBytes, e.CaptureTargetMs,
                     e.CaptureBytesPulled,
                     e.CaptureUnderrunBytes, e.CaptureUnderrunCalls,
                     e.CaptureDroppedBytes, e.CaptureWriteCalls,
                     e.RenderBytesPushed, e.RenderOverrunBytes);
        Sleep (250);
    }
    return 0;
}

void usage()
{
    std::printf ("Usage: NLMatrixTransportProbe [--info|--version|--reset]\n"
                 "       NLMatrixTransportProbe --target EP MS\n"
                 "       NLMatrixTransportProbe --write EP SECONDS [--stall MS] [--chunk-ms MS] [--tone-hz HZ] [--amp A]\n"
                 "       NLMatrixTransportProbe --read EP SECONDS\n"
                 "       NLMatrixTransportProbe --monitor EP SECONDS\n");
}
} // namespace

int main (int argc, char** argv)
{
    if (! openDevice())
        return 1;

    int result = 0;
    if (argc <= 1)
    {
        NLMATRIX_INFO info {};
        result = getInfo (info) ? (printInfo (info), 0) : 1;
    }
    else if (std::strcmp (argv[1], "--version") == 0)
    {
        NLMATRIX_INFO info {};
        if (! getInfo (info))
            return 1;
        printInfo (info);
        const bool ok = info.Version == NLMATRIX_ABI_VERSION
                     && info.EndpointCount == NLMATRIX_MAX_ENDPOINTS
                     && info.Endpoints[0].Size == sizeof (NLMATRIX_ENDPOINT_INFO);
        std::printf ("RESULT abiVersion=%lu expected=%lu endpointCount=%lu endpointStructBytes=%lu\n",
                     info.Version, (unsigned long) NLMATRIX_ABI_VERSION,
                     info.EndpointCount, info.Endpoints[0].Size);
        std::printf ("ABIV3_%s\n", ok ? "OK" : "FAIL");
        result = ok ? 0 : 2;
    }
    else if (std::strcmp (argv[1], "--info") == 0)
    {
        NLMATRIX_INFO info {};
        result = getInfo (info) ? (printInfo (info), 0) : 1;
    }
    else if (std::strcmp (argv[1], "--reset") == 0)
    {
        result = resetTransport() ? 0 : 1;
    }
    else if (std::strcmp (argv[1], "--target") == 0 && argc >= 4)
    {
        const unsigned long endpoint = std::strtoul (argv[2], nullptr, 10);
        const unsigned long targetMs = std::strtoul (argv[3], nullptr, 10);
        if (! setCaptureTarget (endpoint, targetMs))
            return 1;
        NLMATRIX_INFO info {};
        if (! getInfo (info))
            return 1;
        std::printf ("SET_CAPTURE_TARGET ep=%lu requested=%lu\n", endpoint, targetMs);
        printEndpoint (endpoint, info.Endpoints[std::min<unsigned long> (endpoint, NLMATRIX_MAX_ENDPOINTS - 1)]);
        std::printf ("RESULT target ep=%lu targetMs=%lu targetBytes=%lu\n", endpoint,
                     info.Endpoints[endpoint].CaptureTargetMs,
                     info.Endpoints[endpoint].CaptureTargetBytes);
    }
    else if (std::strcmp (argv[1], "--write") == 0 && argc >= 4)
    {
        const unsigned long endpoint = std::strtoul (argv[2], nullptr, 10);
        const double seconds = std::atof (argv[3]);
        double stallMs = 0.0;
        double chunkMs = 10.0;
        double toneHz = 1000.0;
        double amplitude = 0.25;
        for (int i = 4; i + 1 < argc; i += 2)
        {
            const std::string flag (argv[i]);
            if (flag == "--stall") stallMs = std::atof (argv[i + 1]);
            else if (flag == "--chunk-ms") chunkMs = std::atof (argv[i + 1]);
            else if (flag == "--tone-hz") toneHz = std::atof (argv[i + 1]);
            else if (flag == "--amp") amplitude = std::atof (argv[i + 1]);
        }
        result = runWrite (endpoint, seconds, stallMs, chunkMs, toneHz, amplitude);
    }
    else if (std::strcmp (argv[1], "--read") == 0 && argc >= 4)
    {
        result = runRead (std::strtoul (argv[2], nullptr, 10), std::atof (argv[3]));
    }
    else if (std::strcmp (argv[1], "--monitor") == 0 && argc >= 4)
    {
        result = runMonitor (std::strtoul (argv[2], nullptr, 10), std::atof (argv[3]));
    }
    else
    {
        usage();
        result = 3;
    }

    closeDevice();
    return result;
}
