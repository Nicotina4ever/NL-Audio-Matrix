#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstdio>
#include <string>

struct DeviceShareMode;

MIDL_INTERFACE ("F8679F50-850A-41CF-9C72-430F290290C8")
IPolicyConfig : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat (PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat (PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat (PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat (PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod (PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod (PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode (PCWSTR, DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode (PCWSTR, DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue (PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue (PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint (PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility (PCWSTR, INT) = 0;
};

class DECLSPEC_UUID ("870AF99C-171D-4F9E-AF0D-E63DF40C2BC9") CPolicyConfigClient;

static bool makeDefault (IMMDevice* device)
{
    LPWSTR id = nullptr;
    if (FAILED (device->GetId (&id)) || id == nullptr)
        return false;

    IPolicyConfig* policy = nullptr;
    const HRESULT hr = CoCreateInstance (__uuidof (CPolicyConfigClient), nullptr, CLSCTX_ALL,
                                         __uuidof (IPolicyConfig),
                                         reinterpret_cast<void**> (&policy));
    bool ok = false;
    if (SUCCEEDED (hr) && policy != nullptr)
    {
        const HRESULT a = policy->SetDefaultEndpoint (id, eConsole);
        const HRESULT b = policy->SetDefaultEndpoint (id, eMultimedia);
        const HRESULT c = policy->SetDefaultEndpoint (id, eCommunications);
        std::wprintf (L"    default result: 0x%08lx 0x%08lx 0x%08lx\n",
                      (unsigned long) a, (unsigned long) b, (unsigned long) c);
        ok = SUCCEEDED (a) && SUCCEEDED (b) && SUCCEEDED (c);
        policy->Release();
    }
    CoTaskMemFree (id);
    return ok;
}

static std::wstring friendlyName (IMMDevice* device)
{
    IPropertyStore* store = nullptr;
    std::wstring result;
    if (SUCCEEDED (device->OpenPropertyStore (STGM_READ, &store)))
    {
        PROPVARIANT value;
        PropVariantInit (&value);
        if (SUCCEEDED (store->GetValue (PKEY_Device_FriendlyName, &value))
            && value.vt == VT_LPWSTR && value.pwszVal != nullptr)
            result = value.pwszVal;
        PropVariantClear (&value);
        store->Release();
    }
    return result;
}

int main (int argc, char** argv)
{
    const bool set = argc > 1 && std::string (argv[1]) == "--set";
    const bool setDefault = argc > 1 && std::string (argv[1]) == "--default";
    const bool mute = argc > 2 && std::string (argv[2]) == "mute";
    CoInitializeEx (nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* collection = nullptr;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof (IMMDeviceEnumerator),
                                  reinterpret_cast<void**> (&enumerator)))
        || FAILED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &collection)))
    {
        std::printf ("Cannot enumerate render endpoints.\n");
        return 1;
    }

    UINT count = 0;
    collection->GetCount (&count);
    for (UINT i = 0; i < count; ++i)
    {
        IMMDevice* device = nullptr;
        if (FAILED (collection->Item (i, &device)))
            continue;
        const auto name = friendlyName (device);
        IAudioEndpointVolume* volume = nullptr;
        if (SUCCEEDED (device->Activate (__uuidof (IAudioEndpointVolume), CLSCTX_ALL,
                                         nullptr, reinterpret_cast<void**> (&volume))))
        {
            float scalar = 0.0f;
            BOOL isMuted = FALSE;
            volume->GetMasterVolumeLevelScalar (&scalar);
            volume->GetMute (&isMuted);
            std::wprintf (L"[%u] %s  volume=%.3f mute=%d\n", i, name.c_str(), scalar, isMuted ? 1 : 0);
            if ((set || setDefault) && name.find (L"NLMatrix") != std::wstring::npos)
            {
                if (set)
                {
                    volume->SetMute (mute ? TRUE : FALSE, nullptr);
                    if (! mute)
                        volume->SetMasterVolumeLevelScalar (1.0f, nullptr);
                    std::wprintf (L"    -> set volume=%.3f mute=%d\n", mute ? 0.0f : 1.0f, mute ? 1 : 0);
                }
                if (setDefault)
                {
                    const bool ok = makeDefault (device);
                    std::wprintf (L"    -> default endpoint: %s\n", ok ? L"OK" : L"FAILED");
                }
            }
            volume->Release();
        }
        device->Release();
    }

    collection->Release();
    enumerator->Release();
    CoUninitialize();
    return 0;
}
