/*
    Microsoft Application Loopback capture.

    Captures the audio rendered by one process and its child processes, even if
    they render to the same physical/virtual output device. This is different
    from endpoint loopback: two applications sharing "Speakers" can be captured
    as two independent sources.

    Requires Windows 10 build 20348+ and the Windows SDK process-loopback API.
*/

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class ProcessLoopbackCapture
{
public:
    using Sink = std::function<void (const float* left, const float* right, int numFrames)>;

    ProcessLoopbackCapture();
    ~ProcessLoopbackCapture();

    ProcessLoopbackCapture (const ProcessLoopbackCapture&) = delete;
    ProcessLoopbackCapture& operator= (const ProcessLoopbackCapture&) = delete;

    /** Finds the first process with this executable name (case-insensitive). */
    static bool findProcessIdByName (const std::wstring& exeName, unsigned long& pid);

    /** Opens the process tree in loopback mode and starts a capture thread. */
    bool start (unsigned long targetProcessId,
                bool includeProcessTree,
                double requestedSampleRate,
                Sink sink,
                std::string& error);

    void stop();

    double getSampleRate() const;
    int getChannelCount() const;
    bool isRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
