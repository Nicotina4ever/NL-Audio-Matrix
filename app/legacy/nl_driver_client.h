#pragma once

#include <string>
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
    bool writeCapture (int endpointIndex, const void* source, int bytes, std::string& error) const;
    bool reset (std::string& error) const;

private:
    HANDLE device = INVALID_HANDLE_VALUE;
};
