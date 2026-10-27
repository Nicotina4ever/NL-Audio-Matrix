#include "process_loopback.h"

// The process-loopback activation structures are gated on this NTDDI level.
#ifndef NTDDI_VERSION
    #define NTDDI_VERSION 0x0A00000A // NTDDI_WIN10_FE
#endif
#ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace
{
    using Microsoft::WRL::ComPtr;

    struct ComScope
    {
        bool initialized = false;
        ComScope()
        {
            const HRESULT hr = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
            initialized = SUCCEEDED (hr);
        }
        ~ComScope()
        {
            if (initialized)
                CoUninitialize();
        }
    };

    std::string hresultText (HRESULT hr)
    {
        char buffer[64] = {};
        std::snprintf (buffer, sizeof (buffer), "0x%08lX", (unsigned long) hr);
        return buffer;
    }
} // namespace

//==============================================================================
class ProcessLoopbackActivationHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IActivateAudioInterfaceCompletionHandler,
          Microsoft::WRL::FtmBase>
{
public:
    ProcessLoopbackActivationHandler()
        : event (CreateEventW (nullptr, FALSE, FALSE, nullptr))
    {
    }

    ~ProcessLoopbackActivationHandler()
    {
        if (event != nullptr)
            CloseHandle (event);
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted (IActivateAudioInterfaceAsyncOperation* operation) override
    {
        HRESULT activateResult = E_FAIL;
        ComPtr<IUnknown> unknown;
        const HRESULT hr = operation != nullptr
                               ? operation->GetActivateResult (&activateResult, &unknown)
                               : E_POINTER;

        if (SUCCEEDED (hr) && SUCCEEDED (activateResult))
            unknown.As (&client);

        result = SUCCEEDED (hr) ? activateResult : hr;
        if (event != nullptr)
            SetEvent (event);
        return S_OK;
    }

    HANDLE event = nullptr;
    ComPtr<IAudioClient> client;
    HRESULT result = E_FAIL;
};

//==============================================================================
struct ProcessLoopbackCapture::Impl
{
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    std::thread thread;
    std::atomic<bool> stopFlag { false };
    std::atomic<bool> running { false };
    Sink sink;
    double sampleRate = 0.0;
    int channelCount = 2;
    std::string error;
    HANDLE sampleEvent = nullptr;
    std::vector<float> left, right;

    void run();
};

ProcessLoopbackCapture::ProcessLoopbackCapture() : impl (std::make_unique<Impl>()) {}
ProcessLoopbackCapture::~ProcessLoopbackCapture() { stop(); }

bool ProcessLoopbackCapture::findProcessIdByName (const std::wstring& exeName, unsigned long& pid)
{
    if (exeName.empty())
        return false;

    const HANDLE snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof (entry);
    bool found = false;
    if (Process32FirstW (snapshot, &entry))
    {
        do
        {
            if (_wcsicmp (entry.szExeFile, exeName.c_str()) == 0)
            {
                pid = (unsigned long) entry.th32ProcessID;
                found = true;
                break;
            }
        } while (Process32NextW (snapshot, &entry));
    }
    CloseHandle (snapshot);
    return found;
}

bool ProcessLoopbackCapture::start (unsigned long targetProcessId,
                                    bool includeProcessTree,
                                    double requestedSampleRate,
                                    Sink sink,
                                    std::string& error)
{
    stop();
    error.clear();
    if (impl == nullptr)
        impl = std::make_unique<Impl>();

    // ActivateAudioInterfaceAsync for process loopback must run in an MTA.
    // JUCE may already have initialized the main thread as STA (OLE), so use a
    // short-lived MTA worker and wait for the activation before returning.
    std::string activationError;
    std::thread activationThread ([&]
    {
        ComScope com;

        AUDIOCLIENT_ACTIVATION_PARAMS activation = {};
        activation.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        activation.ProcessLoopbackParams.TargetProcessId = (DWORD) targetProcessId;
        // Windows exposes only whole-tree inclusion or whole-tree exclusion.
        // Excluding the target tree would capture every other application,
        // which is not what a per-process source means; always include it.
        (void) includeProcessTree;
        activation.ProcessLoopbackParams.ProcessLoopbackMode =
            PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT activationParams = {};
        activationParams.vt = VT_BLOB;
        activationParams.blob.cbSize = sizeof (activation);
        activationParams.blob.pBlobData = reinterpret_cast<BYTE*> (&activation);

        ComPtr<ProcessLoopbackActivationHandler> handler =
            Microsoft::WRL::Make<ProcessLoopbackActivationHandler>();
        if (handler == nullptr || handler->event == nullptr)
        {
            activationError = "CreateEvent failed";
            return;
        }

        ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
        const HRESULT activationHr = ActivateAudioInterfaceAsync (
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
            __uuidof (IAudioClient),
            &activationParams,
            handler.Get(),
            &operation);

        if (FAILED (activationHr))
        {
            activationError = "ActivateAudioInterfaceAsync failed: " + hresultText (activationHr);
            return;
        }

        const DWORD waitResult = WaitForSingleObject (handler->event, 10000);
        const HRESULT result = handler->result;
        if (waitResult == WAIT_OBJECT_0)
            impl->client = handler->client;

        if (waitResult != WAIT_OBJECT_0)
        {
            activationError = "process-loopback activation timed out";
            return;
        }
        if (FAILED (result) || impl->client == nullptr)
        {
            activationError = "process-loopback activation failed: " + hresultText (result);
            return;
        }

        WAVEFORMATEX format = {};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 2;
        format.nSamplesPerSec = (DWORD) requestedSampleRate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        format.cbSize = 0;

        const HRESULT init = impl->client->Initialize (
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK
                | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            0, 0, &format, nullptr);
        if (FAILED (init))
        {
            activationError = "process-loopback Initialize failed: " + hresultText (init);
            return;
        }

        if (FAILED (impl->client->GetService (__uuidof (IAudioCaptureClient), &impl->capture)))
        {
            activationError = "process-loopback GetService failed";
            return;
        }

        impl->sampleEvent = CreateEventW (nullptr, FALSE, FALSE, nullptr);
        if (impl->sampleEvent == nullptr)
        {
            activationError = "CreateEvent failed";
            return;
        }
        const HRESULT eventHr = impl->client->SetEventHandle (impl->sampleEvent);
        if (FAILED (eventHr))
        {
            activationError = "process-loopback SetEventHandle failed: " + hresultText (eventHr);
            return;
        }

        const HRESULT startHr = impl->client->Start();
        if (FAILED (startHr))
        {
            activationError = "process-loopback Start failed: " + hresultText (startHr);
            return;
        }
    });
    activationThread.join();

    if (! activationError.empty())
    {
        error = activationError;
        impl->capture.Reset();
        impl->client.Reset();
        return false;
    }

    impl->sampleRate = requestedSampleRate;
    impl->channelCount = 2;
    impl->sink = std::move (sink);
    impl->stopFlag.store (false, std::memory_order_relaxed);
    impl->running.store (true, std::memory_order_release);
    impl->thread = std::thread ([this] { impl->run(); });
    return true;
}

void ProcessLoopbackCapture::Impl::run()
{
    ComScope com;

    while (! stopFlag.load (std::memory_order_acquire))
    {
        if (capture == nullptr)
            break;
        if (sampleEvent != nullptr)
            WaitForSingleObject (sampleEvent, 50);
        else
            std::this_thread::sleep_for (std::chrono::milliseconds (2));

        for (;;)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;

            const HRESULT hr = capture->GetBuffer (&data, &frames, &flags,
                                                   &devicePosition, &qpcPosition);
            if (hr != S_OK)
                break;

            left.resize (frames);
            right.resize (frames);
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
            {
                std::fill (left.begin(), left.end(), 0.0f);
                std::fill (right.begin(), right.end(), 0.0f);
            }
            else
            {
                const auto* samples = reinterpret_cast<const float*>(data);
                for (UINT32 i = 0; i < frames; ++i)
                {
                    left[(size_t) i] = samples[(size_t) i * 2];
                    right[(size_t) i] = samples[(size_t) i * 2 + 1];
                }
            }

            if (sink)
                sink (left.data(), right.data(), (int) frames);
            capture->ReleaseBuffer (frames);
        }
    }

    running.store (false, std::memory_order_release);
}

void ProcessLoopbackCapture::stop()
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
    if (impl->sampleEvent != nullptr)
    {
        CloseHandle (impl->sampleEvent);
        impl->sampleEvent = nullptr;
    }
    impl->sink = {};
    impl->running.store (false, std::memory_order_release);
}

double ProcessLoopbackCapture::getSampleRate() const { return impl != nullptr ? impl->sampleRate : 0.0; }
int ProcessLoopbackCapture::getChannelCount() const { return impl != nullptr ? impl->channelCount : 0; }
bool ProcessLoopbackCapture::isRunning() const { return impl != nullptr && impl->running.load(); }
