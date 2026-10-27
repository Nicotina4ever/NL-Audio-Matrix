#pragma once

#include <sysvad.h>

NTSTATUS NlTransportInitialize (_In_ PDRIVER_OBJECT DriverObject);
VOID NlTransportCleanup();
VOID NlTransportSetFilterCounts (_In_ ULONG RenderFilters, _In_ ULONG CaptureFilters);

// Called from WaveRT stream initialization and from the audio timer path.
VOID NlTransportSetFormat (_In_ BOOLEAN Capture, _In_ ULONG EndpointIndex,
                           _In_ const WAVEFORMATEX* Format);
VOID NlTransportReset();

// Render (application -> driver ring -> router).
ULONG NlTransportPushRender (_In_ ULONG EndpointIndex,
                             _In_reads_bytes_(Bytes) const BYTE* Data, _In_ ULONG Bytes);

// Render loopback (render ring -> loopback pin -> application), non-consuming.
VOID NlTransportPeekRender (_In_ ULONG EndpointIndex,
                            _Out_writes_bytes_(Bytes) BYTE* Destination, _In_ ULONG Bytes);

// Capture (router ring -> driver -> application).
VOID NlTransportPullCapture (_In_ ULONG EndpointIndex,
                             _Out_writes_bytes_(Bytes) BYTE* Destination, _In_ ULONG Bytes);
