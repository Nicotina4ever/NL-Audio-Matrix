#include "nl_driver_client.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace
{
    std::string lastErrorText (const char* operation)
    {
        return std::string (operation) + " failed (Win32 " + std::to_string (GetLastError()) + ")";
    }
}

NlDriverClient::~NlDriverClient()
{
    close();
}

bool NlDriverClient::open (std::string& error)
{
    close();
    device = CreateFileW (NLMATRIX_USER_PATH,
                          GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr,
                          OPEN_EXISTING,
                          0,
                          nullptr);
    if (device == INVALID_HANDLE_VALUE)
    {
        error = lastErrorText ("CreateFile(\\\\.\\NLMatrixAudio)");
        return false;
    }
    return true;
}

void NlDriverClient::close()
{
    if (device != INVALID_HANDLE_VALUE)
    {
        CloseHandle (device);
        device = INVALID_HANDLE_VALUE;
    }
}

bool NlDriverClient::getInfo (NLMATRIX_INFO& info, std::string& error) const
{
    DWORD bytes = 0;
    if (! DeviceIoControl (device, NLMATRIX_IOCTL_GET_INFO, nullptr, 0u,
                           &info, static_cast<DWORD> (sizeof (info)), &bytes, nullptr))
    {
        error = lastErrorText ("NLMATRIX_IOCTL_GET_INFO");
        return false;
    }
    // Legacy-engine comparison build: accept both the ABI v2 (228 byte) and
    // ABI v3 (308 byte) GET_INFO layouts.  The legacy engine only reads the
    // common v2 fields; the caller zero-initialises NLMATRIX_INFO.
    if (bytes < static_cast<DWORD> (offsetof (NLMATRIX_INFO, Endpoints)))
    {
        error = "NLMatrixAudio ABI mismatch: GET_INFO returned "
                + std::to_string ((unsigned long) bytes) + " bytes";
        return false;
    }
    return true;
}

int NlDriverClient::readRender (int endpointIndex, void* destination, int maxBytes,
                                std::string& error) const
{
    NLMATRIX_ENDPOINT_REQUEST request {};
    request.EndpointIndex = (unsigned long) (endpointIndex > 0 ? endpointIndex : 0);
    DWORD bytes = 0;
    if (! DeviceIoControl (device, NLMATRIX_IOCTL_READ_RENDER, &request, sizeof (request),
                           destination, static_cast<DWORD> (maxBytes), &bytes, nullptr))
    {
        error = lastErrorText ("NLMATRIX_IOCTL_READ_RENDER");
        return -1;
    }
    return (int) bytes;
}

bool NlDriverClient::writeCapture (int endpointIndex, const void* source, int bytes,
                                   std::string& error) const
{
    if (bytes <= 0)
        return true;
    std::vector<unsigned char> packet (sizeof (NLMATRIX_WRITE_REQUEST) + (size_t) bytes);
    auto* request = reinterpret_cast<NLMATRIX_WRITE_REQUEST*> (packet.data());
    request->EndpointIndex = (unsigned long) (endpointIndex > 0 ? endpointIndex : 0);
    request->DataBytes = (unsigned long) bytes;
    std::memcpy (packet.data() + sizeof (NLMATRIX_WRITE_REQUEST), source, (size_t) bytes);
    DWORD written = 0;
    if (! DeviceIoControl (device, NLMATRIX_IOCTL_WRITE_CAPTURE,
                           packet.data(), static_cast<DWORD> (packet.size()),
                           nullptr, 0u, &written, nullptr))
    {
        error = lastErrorText ("NLMATRIX_IOCTL_WRITE_CAPTURE");
        return false;
    }
    return true;
}

bool NlDriverClient::reset (std::string& error) const
{
    DWORD bytes = 0;
    if (! DeviceIoControl (device, NLMATRIX_IOCTL_RESET, nullptr, 0u,
                           nullptr, 0u, &bytes, nullptr))
    {
        error = lastErrorText ("NLMATRIX_IOCTL_RESET");
        return false;
    }
    return true;
}
