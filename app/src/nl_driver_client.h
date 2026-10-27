#pragma once

#include <string>
#include <vector>
#include <windows.h>

#include "../../driver/sysvad-nl/nlmatrix_ioctl.h"

class NlDriverClient
{
public:
    NlDriverClient() = default;
    ~NlDriverClient();

    NlDriverClient (const NlDriverClient&) = delete;
    NlDriverClient& operator= (const NlDriverClient&) = delete;

    bool open (std::string& error);
    void close();
    bool isOpen() const { return device != INVALID_HANDLE_VALUE; }

    bool getInfo (NLMATRIX_INFO& info, std::string& error) const;
    int readRender (int endpointIndex, void* destination, int maxBytes, std::string& error) const;
    bool writeCapture (int endpointIndex, const void* source, int bytes, std::string& error);
    bool reset (std::string& error) const;

    // ABI v3: configure the per-endpoint kernel capture queue target.
    // Returns false (with error text) when the installed driver predates ABI v3;
    // callers must keep working with the driver-provided default in that case.
    bool setCaptureTarget (int endpointIndex, unsigned long targetMs, std::string& error);

private:
    HANDLE device = INVALID_HANDLE_VALUE;
    // Reused IOCTL packet for writeCapture: the audio feeder thread must not
    // allocate on every write.
    std::vector<unsigned char> writePacket;
};
