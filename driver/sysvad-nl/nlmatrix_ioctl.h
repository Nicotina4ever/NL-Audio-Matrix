#pragma once

//
// Shared user/kernel ABI for the NLMatrix virtual audio transport.
// Keep this header C-compatible and free of pointer-sized fields.
//
// The transport exposes a fixed pool of independent render/capture endpoint
// pairs.  Pair N is a single Windows render endpoint and a single Windows
// capture endpoint.  Endpoint indexes are stable and are assigned by the
// driver minipair tables; unused indexes are reported as inactive.
//
// ABI history
//   v1  initial transport: GET_INFO, READ_RENDER, WRITE_CAPTURE, RESET.
//   v2  CaptureUsedBytes / CaptureTargetBytes / CaptureDroppedBytes gauges.
//   v3  accurate DMA pull/write accounting, capture underrun and render
//       overrun evidence, and the runtime per-endpoint capture target
//       (SET_CAPTURE_TARGET).  The new fields are APPENDED to
//       NLMATRIX_ENDPOINT_INFO in the order agreed with the user-mode writer:
//       the v2 field offsets are unchanged, but a v2 client must be rebuilt
//       against this header because NLMATRIX_INFO grew (a too-small GET_INFO
//       output buffer is rejected).
//

#ifdef _KERNEL_MODE
    #include <ntddk.h>
    #include <devioctl.h>
#else
    #include <windows.h>
    #include <winioctl.h>
#endif

#define NLMATRIX_DEVICE_NAME        L"\\Device\\NLMatrixAudio"
#define NLMATRIX_SYMLINK_NAME       L"\\DosDevices\\NLMatrixAudio"
#define NLMATRIX_USER_PATH          L"\\\\.\\NLMatrixAudio"

// v3 adds SET_CAPTURE_TARGET and the appended endpoint counters.  Kernel and
// user mode must be rebuilt together from this header.
#define NLMATRIX_ABI_VERSION        3
#define NLMATRIX_DEVICE_TYPE        0x8020
#define NLMATRIX_MAX_ENDPOINTS      4

// Capture target bounds, in milliseconds of queued router audio.  The driver
// clamps SET_CAPTURE_TARGET to this range; the default applies when the
// router has not selected a target for an endpoint.
#define NLMATRIX_CAPTURE_TARGET_MS_MIN      10
#define NLMATRIX_CAPTURE_TARGET_MS_MAX      500
#define NLMATRIX_CAPTURE_TARGET_MS_DEFAULT  30

#define NLMATRIX_IOCTL_GET_INFO \
    CTL_CODE(NLMATRIX_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define NLMATRIX_IOCTL_READ_RENDER \
    CTL_CODE(NLMATRIX_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define NLMATRIX_IOCTL_WRITE_CAPTURE \
    CTL_CODE(NLMATRIX_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define NLMATRIX_IOCTL_RESET \
    CTL_CODE(NLMATRIX_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
// ABI v3: select how much router audio may be queued for the Windows capture
// client of one endpoint pair.  No output buffer; read back the effective
// value with GET_INFO (CaptureTargetMs / CaptureTargetBytes).
#define NLMATRIX_IOCTL_SET_CAPTURE_TARGET \
    CTL_CODE(NLMATRIX_DEVICE_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 8)

typedef struct _NLMATRIX_STREAM_INFO
{
    unsigned long SampleRate;
    unsigned long Channels;
    unsigned long BitsPerSample;
    unsigned long IsFloat;
    unsigned long BlockAlign;
} NLMATRIX_STREAM_INFO;

typedef struct _NLMATRIX_ENDPOINT_INFO
{
    unsigned long Size;
    NLMATRIX_STREAM_INFO Render;   // application -> router
    NLMATRIX_STREAM_INFO Capture;  // router -> application
    unsigned long RenderCapacityBytes;
    unsigned long CaptureCapacityBytes;
    unsigned long RenderBytesPushed;
    unsigned long CaptureBytesPulled;
    unsigned long RenderPeak16;
    unsigned long CapturePeak16;
    unsigned long CaptureUsedBytes;
    unsigned long CaptureTargetBytes;
    unsigned long CaptureDroppedBytes;
    unsigned long Active;          // endpoint pair present in this driver

    // ---- ABI v3: appended instrumentation, do not reorder (writer contract) ----
    unsigned long CaptureTargetMs;         // configured target, clamped 10..500
    unsigned long CaptureWriteCalls;       // accepted WRITE_CAPTURE calls
    unsigned long CaptureUnderrunBytes;    // capture DMA bytes requested while
                                           // the router ring was short
    unsigned long CaptureUnderrunCalls;    // pull calls that underran
    unsigned long RenderOverrunBytes;      // render ring bytes dropped because
                                           // the router did not drain in time
} NLMATRIX_ENDPOINT_INFO;

typedef struct _NLMATRIX_INFO
{
    unsigned long Size;
    unsigned long Version;
    unsigned long EndpointCount;
    unsigned long RenderFilterCount;
    unsigned long CaptureFilterCount;
    NLMATRIX_ENDPOINT_INFO Endpoints[NLMATRIX_MAX_ENDPOINTS];
} NLMATRIX_INFO;

// Input for NLMATRIX_IOCTL_WRITE_CAPTURE: this header followed by DataBytes
// bytes of raw audio.  EndpointIndex selects the capture endpoint pair.
typedef struct _NLMATRIX_WRITE_REQUEST
{
    unsigned long EndpointIndex;
    unsigned long DataBytes;
} NLMATRIX_WRITE_REQUEST;

// Input for NLMATRIX_IOCTL_READ_RENDER and NLMATRIX_IOCTL_RESET.
typedef struct _NLMATRIX_ENDPOINT_REQUEST
{
    unsigned long EndpointIndex;
} NLMATRIX_ENDPOINT_REQUEST;

// Input for NLMATRIX_IOCTL_SET_CAPTURE_TARGET (ABI v3).
// CaptureTargetMs is clamped to [NLMATRIX_CAPTURE_TARGET_MS_MIN,
// NLMATRIX_CAPTURE_TARGET_MS_MAX]; the driver then recomputes the byte target
// from the live capture format and drops only the excess audio already queued.
typedef struct _NLMATRIX_CAPTURE_TARGET_REQUEST
{
    unsigned long EndpointIndex;
    unsigned long CaptureTargetMs;
} NLMATRIX_CAPTURE_TARGET_REQUEST;

#pragma pack(pop)
