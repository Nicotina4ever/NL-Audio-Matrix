/*
    Native WASAPI render-endpoint loopback capture.

    JUCE's WASAPI backend does not expose AUDCLNT_STREAMFLAGS_LOOPBACK, so this
    small helper uses the Windows Core Audio API directly. It captures whatever
    a render endpoint is playing (an application, the system mix, a virtual
    cable, etc.) without requiring a separate capture endpoint like CABLE Output.

    The helper is deliberately independent of VB-Audio, ReaRoute and any other
    third-party driver. It enumerates active render endpoints dynamically.
*/

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class WasapiLoopbackCapture
{
public:
    using Sink = std::function<void (const float* left, const float* right, int numFrames)>;

    WasapiLoopbackCapture();
    ~WasapiLoopbackCapture();

    WasapiLoopbackCapture (const WasapiLoopbackCapture&) = delete;
    WasapiLoopbackCapture& operator= (const WasapiLoopbackCapture&) = delete;

    /** Returns the friendly names of all active WASAPI render endpoints. */
    static std::vector<std::wstring> listRenderEndpoints();

    /** Opens the named render endpoint in loopback mode and starts a capture
        thread. The sink is called with stereo float frames. Returns false and
        fills error on failure. */
    bool start (const std::wstring& endpointName, Sink sink, std::string& error);

    /** Stops the capture thread and releases Core Audio objects. */
    void stop();

    double getSampleRate() const;
    int getChannelCount() const;
    bool isRunning() const;
    /** Friendly name of the render endpoint actually resolved by start(). */
    std::wstring getEndpointName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
