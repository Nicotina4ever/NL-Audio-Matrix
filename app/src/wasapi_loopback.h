/*
    Native WASAPI render-endpoint loopback capture.

    JUCE's WASAPI backend does not expose AUDCLNT_STREAMFLAGS_LOOPBACK, so this
    small helper uses the Windows Core Audio API directly. It captures whatever
    a render endpoint is playing (an application, the system mix, a virtual
    cable, etc.) without requiring a separate capture endpoint like CABLE Output.

    Multi-channel: the endpoint's shared-mode mix format decides the channel
    count (up to 32), so one signed 16-channel virtual cable can carry many
    independent channels into NL Audio Matrix through a single Windows endpoint.

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
    /** Legacy stereo sink: left/right only. */
    using Sink = std::function<void (const float* left, const float* right, int numFrames)>;

    /** Multi-channel sink: all captured channels are delivered separately.
        The pointer array and the sample memory are only valid for the
        duration of the call. */
    using MultiSink = std::function<void (const float* const* channels, int numChannels,
                                          int numFrames)>;

    WasapiLoopbackCapture();
    ~WasapiLoopbackCapture();

    WasapiLoopbackCapture (const WasapiLoopbackCapture&) = delete;
    WasapiLoopbackCapture& operator= (const WasapiLoopbackCapture&) = delete;

    /** Returns the friendly names of all active WASAPI render endpoints. */
    static std::vector<std::wstring> listRenderEndpoints();

    /** Reads one render endpoint's shared-mode mix format without opening a
        stream.  Callers use it to size a multi-channel ring before start().
        Returns false and fills error when the endpoint is not found. */
    static bool probeRenderEndpoint (const std::wstring& endpointName, int& channelCount,
                                     double& sampleRate, int& bitsPerSample,
                                     std::string& error);

    /** Opens the named render endpoint in loopback mode and starts a capture
        thread. The sink is called with stereo float frames. Returns false and
        fills error on failure. */
    bool start (const std::wstring& endpointName, Sink sink, std::string& error);

    /** Multi-channel variant: the sink receives every channel of the shared
        mix format (1..32). */
    bool startMulti (const std::wstring& endpointName, MultiSink sink, std::string& error);

    /** Stops the capture thread and releases Core Audio objects. */
    void stop();

    double getSampleRate() const;
    int getChannelCount() const;
    bool isRunning() const;
    /** Friendly name of the render endpoint actually resolved by start(). */
    std::wstring getEndpointName() const;

private:
    bool startInternal (const std::wstring& endpointName, MultiSink sink, Sink stereo,
                        std::string& error);

    struct Impl;
    std::unique_ptr<Impl> impl;
};
