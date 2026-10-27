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
    Sink sink;
    std::vector<float> left, right;
    double sampleRate = 0.0;
    int channelCount = 0;
    int bitsPerSample = 0;
    bool isFloat = false;
    bool isPcm = false;
    std::string error;

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

WasapiLoopbackCapture::WasapiLoopbackCapture() : impl (std::make_unique<Impl>()) {}
WasapiLoopbackCapture::~WasapiLoopbackCapture() { stop(); }

bool WasapiLoopbackCapture::start (const std::wstring& endpointName, Sink sink, std::string& error)
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

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED (impl->enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &collection)))
    {
        error = "render endpoint enumeration failed";
        return false;
    }

    UINT count = 0;
    collection->GetCount (&count);
    ComPtr<IMMDevice> selected;
    for (UINT i = 0; i < count && selected == nullptr; ++i)
    {
        ComPtr<IMMDevice> device;
        if (FAILED (collection->Item (i, &device)))
            continue;
        const auto friendly = getFriendlyName (device.Get());
        if (friendly == endpointName
            || (endpointName.size() > 0 && friendly.find (endpointName) != std::wstring::npos))
            selected = device;
    }

    if (selected == nullptr)
    {
        error = "render endpoint not found";
        return false;
    }

    impl->device = selected;
    if (FAILED (impl->device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, &impl->client)))
    {
        error = "IAudioClient activation failed";
        return false;
    }

    if (FAILED (impl->client->GetMixFormat (&impl->format)) || impl->format == nullptr)
    {
        error = "GetMixFormat failed";
        return false;
    }

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
    impl->channelCount = (int) impl->format->nChannels;
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

    impl->sink = std::move (sink);
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

            if (frames > 0 && sink)
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
    left.resize (frames);
    right.resize (frames);

    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
    {
        std::fill (left.begin(), left.end(), 0.0f);
        std::fill (right.begin(), right.end(), 0.0f);
        sink (left.data(), right.data(), (int) frames);
        return true;
    }

    const int channels = channelCount;
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

    for (UINT32 i = 0; i < frames; ++i)
    {
        left[i] = sampleAt (i, 0);
        right[i] = channels > 1 ? sampleAt (i, 1) : left[i];
        left[i] = std::clamp (left[i], -1.0f, 1.0f);
        right[i] = std::clamp (right[i], -1.0f, 1.0f);
    }

    sink (left.data(), right.data(), (int) frames);
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
    impl->sink = {};
}

double WasapiLoopbackCapture::getSampleRate() const { return impl != nullptr ? impl->sampleRate : 0.0; }
int WasapiLoopbackCapture::getChannelCount() const { return impl != nullptr ? impl->channelCount : 0; }
bool WasapiLoopbackCapture::isRunning() const { return impl != nullptr && impl->running.load(); }
