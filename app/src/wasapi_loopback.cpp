#include "wasapi_loopback.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace
{
    using Microsoft::WRL::ComPtr;

    constexpr REFERENCE_TIME kBufferDuration = 1000000; // 100 ms
    constexpr int kMaxChannels = 32;

    const GUID kSubtypeFloat = { 0x00000003, 0x0000, 0x0010,
                                 { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
    const GUID kSubtypePcm   = { 0x00000001, 0x0000, 0x0010,
                                 { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

    std::string hresultText (HRESULT hr)
    {
        char buffer[64] = {};
        std::snprintf (buffer, sizeof (buffer), "0x%08lX", (unsigned long) hr);
        return buffer;
    }

    struct ComScope
    {
        bool initialized = false;
        ComScope()
        {
            const HRESULT hr = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
            initialized = SUCCEEDED (hr);
            // RPC_E_CHANGED_MODE means another apartment already exists; that
            // is fine as long as the caller keeps its own COM lifetime.
        }
        ~ComScope()
        {
            if (initialized)
                CoUninitialize();
        }
    };

    std::wstring getFriendlyName (IMMDevice* device)
    {
        ComPtr<IPropertyStore> store;
        if (device == nullptr || FAILED (device->OpenPropertyStore (STGM_READ, &store)))
            return {};

        PROPVARIANT value;
        PropVariantInit (&value);
        std::wstring result;
        if (SUCCEEDED (store->GetValue (PKEY_Device_FriendlyName, &value))
            && value.vt == VT_LPWSTR && value.pwszVal != nullptr)
            result = value.pwszVal;
        PropVariantClear (&value);
        return result;
    }

    std::vector<std::wstring> enumerateRenderNames (IMMDeviceEnumerator* enumerator)
    {
        std::vector<std::wstring> names;
        ComPtr<IMMDeviceCollection> collection;
        if (enumerator == nullptr
            || FAILED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &collection)))
            return names;

        UINT count = 0;
        if (FAILED (collection->GetCount (&count)))
            return names;

        for (UINT i = 0; i < count; ++i)
        {
            ComPtr<IMMDevice> device;
            if (SUCCEEDED (collection->Item (i, &device)))
            {
                auto name = getFriendlyName (device.Get());
                if (! name.empty())
                    names.push_back (std::move (name));
            }
        }
        return names;
    }

    // Exact match first, then a substring match, so "CABLE Input" resolves the
    // endpoint whose friendly name is "CABLE Input (VB-Audio Virtual Cable)".
    ComPtr<IMMDevice> findRenderDevice (IMMDeviceEnumerator* enumerator, const std::wstring& wanted)
    {
        ComPtr<IMMDevice> exact, partial;
        if (enumerator == nullptr)
            return {};

        ComPtr<IMMDeviceCollection> collection;
        if (FAILED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &collection)))
            return {};

        UINT count = 0;
        if (FAILED (collection->GetCount (&count)))
            return {};

        for (UINT i = 0; i < count; ++i)
        {
            ComPtr<IMMDevice> device;
            if (FAILED (collection->Item (i, &device)))
                continue;
            const auto friendly = getFriendlyName (device.Get());
            if (friendly.empty())
                continue;
            if (friendly == wanted)
                return device;
            if (partial == nullptr && ! wanted.empty()
                && friendly.find (wanted) != std::wstring::npos)
                partial = device;
        }
        return partial;
    }

    bool readMixFormat (IAudioClient* client, WAVEFORMATEX** format, std::string& error)
    {
        if (client == nullptr || FAILED (client->GetMixFormat (format)) || *format == nullptr)
        {
            error = "GetMixFormat failed";
            return false;
        }
        return true;
    }
} // namespace

struct WasapiLoopbackCapture::Impl
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX* format = nullptr;
    std::thread thread;
    std::atomic<bool> stopFlag { false };
    std::atomic<bool> running { false };
    MultiSink multiSink;
    Sink stereoSink;
    std::vector<std::vector<float>> channelBuffers;
    std::vector<const float*> channelPointers;
    double sampleRate = 0.0;
    int channelCount = 0;
    int bitsPerSample = 0;
    bool isFloat = false;
    bool isPcm = false;
    std::string error;
    std::wstring resolvedName;

    void run();
    bool convertPacket (const BYTE* data, UINT32 frames, DWORD flags);
};

std::vector<std::wstring> WasapiLoopbackCapture::listRenderEndpoints()
{
    ComScope com;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS (&enumerator))))
        return {};
    return enumerateRenderNames (enumerator.Get());
}

bool WasapiLoopbackCapture::probeRenderEndpoint (const std::wstring& endpointName,
                                                 int& channelCount, double& sampleRate,
                                                 int& bitsPerSample, std::string& error)
{
    channelCount = 0;
    sampleRate = 0.0;
    bitsPerSample = 0;
    error.clear();

    ComScope com;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS (&enumerator))))
    {
        error = "MMDeviceEnumerator creation failed";
        return false;
    }

    auto device = findRenderDevice (enumerator.Get(), endpointName);
    if (device == nullptr)
    {
        error = "render endpoint not found";
        return false;
    }

    ComPtr<IAudioClient> client;
    if (FAILED (device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, &client)))
    {
        error = "IAudioClient activation failed";
        return false;
    }

    WAVEFORMATEX* format = nullptr;
    if (! readMixFormat (client.Get(), &format, error))
        return false;

    channelCount = std::clamp ((int) format->nChannels, 1, kMaxChannels);
    sampleRate = (double) format->nSamplesPerSec;
    bitsPerSample = (int) format->wBitsPerSample;
    CoTaskMemFree (format);
    return true;
}

WasapiLoopbackCapture::WasapiLoopbackCapture() : impl (std::make_unique<Impl>()) {}
WasapiLoopbackCapture::~WasapiLoopbackCapture() { stop(); }

bool WasapiLoopbackCapture::start (const std::wstring& endpointName, Sink sink, std::string& error)
{
    return startInternal (endpointName, {}, std::move (sink), error);
}

bool WasapiLoopbackCapture::startMulti (const std::wstring& endpointName, MultiSink sink,
                                        std::string& error)
{
    return startInternal (endpointName, std::move (sink), {}, error);
}

bool WasapiLoopbackCapture::startInternal (const std::wstring& endpointName, MultiSink multi,
                                           Sink stereo, std::string& error)
{
    stop();
    error.clear();
    if (impl == nullptr)
        impl = std::make_unique<Impl>();

    ComScope com;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS (&impl->enumerator))))
    {
        error = "MMDeviceEnumerator creation failed";
        return false;
    }

    auto selected = findRenderDevice (impl->enumerator.Get(), endpointName);
    if (selected == nullptr)
    {
        error = "render endpoint not found";
        return false;
    }

    impl->device = selected;
    impl->resolvedName = getFriendlyName (impl->device.Get());
    if (FAILED (impl->device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, &impl->client)))
    {
        error = "IAudioClient activation failed";
        return false;
    }

    if (! readMixFormat (impl->client.Get(), &impl->format, error))
        return false;

    const HRESULT init = impl->client->Initialize (AUDCLNT_SHAREMODE_SHARED,
                                                   AUDCLNT_STREAMFLAGS_LOOPBACK,
                                                   kBufferDuration, 0,
                                                   impl->format, nullptr);
    if (FAILED (init))
    {
        error = "loopback Initialize failed: " + hresultText (init);
        return false;
    }

    if (FAILED (impl->client->GetService (__uuidof (IAudioCaptureClient), &impl->capture)))
    {
        error = "IAudioCaptureClient service failed";
        return false;
    }

    impl->sampleRate = (double) impl->format->nSamplesPerSec;
    impl->channelCount = std::clamp ((int) impl->format->nChannels, 1, kMaxChannels);
    impl->bitsPerSample = (int) impl->format->wBitsPerSample;
    impl->isFloat = impl->format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    impl->isPcm = impl->format->wFormatTag == WAVE_FORMAT_PCM;

    if (impl->format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && impl->format->cbSize >= 22)
    {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(impl->format);
        impl->isFloat = IsEqualGUID (ext->SubFormat, kSubtypeFloat);
        impl->isPcm = IsEqualGUID (ext->SubFormat, kSubtypePcm);
    }

    if (FAILED (impl->client->Start()))
    {
        error = "loopback Start failed";
        return false;
    }

    impl->channelBuffers.assign ((size_t) impl->channelCount, {});
    impl->channelPointers.assign ((size_t) impl->channelCount, nullptr);
    impl->multiSink = std::move (multi);
    impl->stereoSink = std::move (stereo);
    impl->stopFlag.store (false, std::memory_order_relaxed);
    impl->running.store (true, std::memory_order_release);
    impl->thread = std::thread ([this] { impl->run(); });
    return true;
}

void WasapiLoopbackCapture::Impl::run()
{
    ComScope com;

    while (! stopFlag.load (std::memory_order_acquire))
    {
        UINT32 packetFrames = 0;
        const HRESULT next = capture != nullptr ? capture->GetNextPacketSize (&packetFrames)
                                                : E_FAIL;
        if (FAILED (next))
        {
            error = "GetNextPacketSize failed: " + hresultText (next);
            break;
        }

        while (packetFrames > 0 && ! stopFlag.load (std::memory_order_acquire))
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;

            const HRESULT got = capture->GetBuffer (&data, &frames, &flags,
                                                    &devicePosition, &qpcPosition);
            if (FAILED (got))
            {
                error = "GetBuffer failed: " + hresultText (got);
                stopFlag.store (true, std::memory_order_release);
                break;
            }

            if (frames > 0 && (multiSink || stereoSink))
                convertPacket (data, frames, flags);

            capture->ReleaseBuffer (frames);
            if (FAILED (capture->GetNextPacketSize (&packetFrames)))
                break;
        }

        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }

    running.store (false, std::memory_order_release);
}

bool WasapiLoopbackCapture::Impl::convertPacket (const BYTE* data, UINT32 frames, DWORD flags)
{
    const int channels = std::clamp (channelCount, 1, kMaxChannels);
    if ((int) channelBuffers.size() != channels)
        channelBuffers.assign ((size_t) channels, {});
    if ((int) channelPointers.size() != channels)
        channelPointers.assign ((size_t) channels, nullptr);

    for (auto& buffer : channelBuffers)
        buffer.resize (frames);

    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
    {
        for (auto& buffer : channelBuffers)
            std::fill (buffer.begin(), buffer.end(), 0.0f);
    }
    else
    {
        const int bits = bitsPerSample;

        auto sampleAt = [&] (UINT32 frame, int channel) -> float
        {
            const UINT32 index = frame * (UINT32) channels + (UINT32) channel;
            if (isFloat && bits == 32)
            {
                const float* values = reinterpret_cast<const float*>(data);
                return values[index];
            }
            if (isPcm && bits == 16)
            {
                const int16_t* values = reinterpret_cast<const int16_t*>(data);
                return (float) values[index] / 32768.0f;
            }
            if (isPcm && bits == 24)
            {
                const BYTE* p = data + (size_t) index * 3;
                int32_t value = (int32_t) p[0] | ((int32_t) p[1] << 8) | ((int32_t) p[2] << 16);
                if (value & 0x800000)
                    value |= ~0xFFFFFF;
                return (float) value / 8388608.0f;
            }
            if (isPcm && bits == 32)
            {
                const int32_t* values = reinterpret_cast<const int32_t*>(data);
                return (float) ((double) values[index] / 2147483648.0);
            }
            if (isPcm && bits == 8)
            {
                return ((float) data[index] - 128.0f) / 128.0f;
            }
            return 0.0f;
        };

        for (int ch = 0; ch < channels; ++ch)
        {
            auto& buffer = channelBuffers[(size_t) ch];
            for (UINT32 i = 0; i < frames; ++i)
                buffer[(size_t) i] = std::clamp (sampleAt (i, ch), -1.0f, 1.0f);
        }
    }

    for (int ch = 0; ch < channels; ++ch)
        channelPointers[(size_t) ch] = channelBuffers[(size_t) ch].data();

    if (multiSink)
        multiSink (channelPointers.data(), channels, (int) frames);
    else if (stereoSink)
        stereoSink (channelPointers[0], channels > 1 ? channelPointers[1] : channelPointers[0],
                    (int) frames);
    return true;
}

void WasapiLoopbackCapture::stop()
{
    if (impl == nullptr)
        return;

    impl->stopFlag.store (true, std::memory_order_release);
    if (impl->thread.joinable())
        impl->thread.join();

    if (impl->client != nullptr)
        impl->client->Stop();

    impl->capture.Reset();
    impl->client.Reset();
    impl->device.Reset();
    impl->enumerator.Reset();
    if (impl->format != nullptr)
    {
        CoTaskMemFree (impl->format);
        impl->format = nullptr;
    }
    impl->running.store (false, std::memory_order_release);
    impl->multiSink = {};
    impl->stereoSink = {};
}

double WasapiLoopbackCapture::getSampleRate() const { return impl != nullptr ? impl->sampleRate : 0.0; }
int WasapiLoopbackCapture::getChannelCount() const { return impl != nullptr ? impl->channelCount : 0; }
bool WasapiLoopbackCapture::isRunning() const { return impl != nullptr && impl->running.load(); }

std::wstring WasapiLoopbackCapture::getEndpointName() const
{
    return impl != nullptr ? impl->resolvedName : std::wstring();
}
