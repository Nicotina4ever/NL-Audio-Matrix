#include "nltransport.h"
#include "nlmatrix_ioctl.h"

#include <wdmsec.h>
#include <mmreg.h>

// The pool is deliberately small: a virtual endpoint pair is a real Windows
// device and costs a ring pair, so do not create a pair per routing node.
#define NL_RING_CAPACITY        (256 * 1024)
#define NL_TRANSPORT_POOL_TAG   'TMLN'

// Capture-side target default (milliseconds of router audio kept queued for
// the Windows capture client).  Per-endpoint overrides arrive through
// NLMATRIX_IOCTL_SET_CAPTURE_TARGET; the effective value is kept in
// NLMATRIX_ENDPOINT_INFO.CaptureTargetMs so the write/pull paths never read
// it without the endpoint lock.
#define NL_CAPTURE_TARGET_MS_DEFAULT    NLMATRIX_CAPTURE_TARGET_MS_DEFAULT

typedef struct _NL_RING
{
    PUCHAR Buffer;
    ULONG Capacity;
    ULONG Read;
    ULONG Write;
    ULONG Used;
} NL_RING;

typedef struct _NL_TRANSPORT_ENDPOINT
{
    KSPIN_LOCK Lock;
    NL_RING Render;
    NL_RING Capture;
    NLMATRIX_ENDPOINT_INFO Info;
} NL_TRANSPORT_ENDPOINT;

typedef struct _NL_TRANSPORT
{
    NL_TRANSPORT_ENDPOINT Endpoints[NLMATRIX_MAX_ENDPOINTS];
    BOOLEAN Initialized;
} NL_TRANSPORT;

static NL_TRANSPORT g_Transport;
static ULONG g_RenderFilterCount = 0;
static ULONG g_CaptureFilterCount = 0;
static PDEVICE_OBJECT g_ControlDevice = NULL;
static PDRIVER_DISPATCH g_OldCreate = NULL;
static PDRIVER_DISPATCH g_OldClose = NULL;
static PDRIVER_DISPATCH g_OldCleanup = NULL;
static PDRIVER_DISPATCH g_OldDeviceControl = NULL;

static VOID NlRingInitialize (_Out_ NL_RING* Ring, _In_ ULONG Capacity)
{
    Ring->Buffer = (PUCHAR) ExAllocatePool2 (POOL_FLAG_NON_PAGED, Capacity, NL_TRANSPORT_POOL_TAG);
    Ring->Capacity = Ring->Buffer != NULL ? Capacity : 0;
    Ring->Read = Ring->Write = Ring->Used = 0;
}

static VOID NlRingReset (_Inout_ NL_RING* Ring)
{
    Ring->Read = Ring->Write = Ring->Used = 0;
}

// Returns the number of oldest bytes discarded to make room (overrun).
static ULONG NlRingWrite (_Inout_ NL_RING* Ring, _In_reads_bytes_(Bytes) const BYTE* Data, _In_ ULONG Bytes)
{
    if (Ring->Buffer == NULL || Ring->Capacity == 0 || Bytes == 0)
        return 0;

    ULONG Dropped = 0;

    if (Bytes > Ring->Capacity)
    {
        Data += Bytes - Ring->Capacity;
        Bytes = Ring->Capacity;
    }

    if (Bytes > Ring->Capacity - Ring->Used)
    {
        const ULONG Drop = Bytes - (Ring->Capacity - Ring->Used);
        Ring->Read = (Ring->Read + Drop) % Ring->Capacity;
        Ring->Used -= Drop;
        Dropped = Drop;
    }

    const ULONG First = min (Bytes, Ring->Capacity - Ring->Write);
    RtlCopyMemory (Ring->Buffer + Ring->Write, Data, First);
    if (First < Bytes)
        RtlCopyMemory (Ring->Buffer, Data + First, Bytes - First);

    Ring->Write = (Ring->Write + Bytes) % Ring->Capacity;
    Ring->Used += Bytes;
    return Dropped;
}

static ULONG NlRingRead (_Inout_ NL_RING* Ring, _Out_writes_bytes_(MaxBytes) BYTE* Destination, _In_ ULONG MaxBytes)
{
    if (Ring->Buffer == NULL || Destination == NULL || MaxBytes == 0)
        return 0;

    const ULONG Bytes = min (MaxBytes, Ring->Used);
    if (Bytes == 0)
        return 0;

    const ULONG First = min (Bytes, Ring->Capacity - Ring->Read);
    RtlCopyMemory (Destination, Ring->Buffer + Ring->Read, First);
    if (First < Bytes)
        RtlCopyMemory (Destination + First, Ring->Buffer, Bytes - First);

    Ring->Read = (Ring->Read + Bytes) % Ring->Capacity;
    Ring->Used -= Bytes;
    return Bytes;
}

static ULONG NlRingDropOldest (_Inout_ NL_RING* Ring, _In_ ULONG Bytes)
{
    if (Ring->Buffer == NULL || Ring->Capacity == 0 || Ring->Used == 0)
        return 0;

    const ULONG Drop = min (Bytes, Ring->Used);
    Ring->Read = (Ring->Read + Drop) % Ring->Capacity;
    Ring->Used -= Drop;
    return Drop;
}

static ULONG NlCaptureTargetBytes (_In_ const NLMATRIX_STREAM_INFO* Info, _In_ ULONG TargetMs)
{
    if (Info == NULL || Info->SampleRate == 0 || Info->BlockAlign == 0)
        return 0;

    // Distinct from the ring capacity: keep at most TargetMs of router audio
    // queued for the Windows capture client.  If no client is draining the
    // endpoint, older audio is discarded instead of being replayed much later.
    // TargetMs is the configured (already clamped) value; 0 selects the
    // default so early callers and unconfigured endpoints agree.
    if (TargetMs == 0)
        TargetMs = NL_CAPTURE_TARGET_MS_DEFAULT;
    const ULONGLONG frames = ((ULONGLONG) Info->SampleRate * TargetMs + 999u) / 1000u;
    const ULONGLONG bytes = frames * Info->BlockAlign;
    if (bytes < Info->BlockAlign)
        return Info->BlockAlign;
    return (ULONG) min (bytes, (ULONGLONG) MAXULONG);
}

static ULONG NlRingWriteBounded (_Inout_ NL_RING* Ring,
                                _In_reads_bytes_(Bytes) const BYTE* Data,
                                _In_ ULONG Bytes, _In_ ULONG TargetBytes)
{
    ULONG Dropped = NlRingWrite (Ring, Data, Bytes);
    if (TargetBytes > 0 && Ring->Used > TargetBytes)
        Dropped += NlRingDropOldest (Ring, Ring->Used - TargetBytes);
    return Dropped;
}

static BOOLEAN NlIsFloatFormat (_In_ const WAVEFORMATEX* Format)
{
    if (Format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return TRUE;

    if (Format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && Format->cbSize >= sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX))
    {
        const auto* Extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(Format);
        return IsEqualGUID (Extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? TRUE : FALSE;
    }
    return FALSE;
}

static VOID NlSetStreamInfo (_Out_ NLMATRIX_STREAM_INFO* Info, _In_ const WAVEFORMATEX* Format)
{
    if (Format == NULL)
    {
        Info->SampleRate = 48000;
        Info->Channels = 2;
        Info->BitsPerSample = 16;
        Info->IsFloat = 0;
        Info->BlockAlign = 4;
        return;
    }

    Info->SampleRate = Format->nSamplesPerSec;
    Info->Channels = Format->nChannels;
    Info->BitsPerSample = Format->wBitsPerSample;
    Info->IsFloat = NlIsFloatFormat (Format) ? 1 : 0;
    Info->BlockAlign = Format->nBlockAlign != 0
                           ? Format->nBlockAlign
                           : (Format->nChannels * Format->wBitsPerSample) / 8;
}

static ULONG NlClampEndpoint (_In_ ULONG EndpointIndex)
{
    return EndpointIndex < NLMATRIX_MAX_ENDPOINTS ? EndpointIndex : 0;
}

VOID NlTransportSetFilterCounts (_In_ ULONG RenderFilters, _In_ ULONG CaptureFilters)
{
    g_RenderFilterCount = RenderFilters;
    g_CaptureFilterCount = CaptureFilters;
}

VOID NlTransportReset()
{
    for (ULONG index = 0; index < NLMATRIX_MAX_ENDPOINTS; ++index)
    {
        KIRQL OldIrql;
        KeAcquireSpinLock (&g_Transport.Endpoints[index].Lock, &OldIrql);
        NlRingReset (&g_Transport.Endpoints[index].Render);
        NlRingReset (&g_Transport.Endpoints[index].Capture);
        g_Transport.Endpoints[index].Info.CaptureUsedBytes = 0;
        KeReleaseSpinLock (&g_Transport.Endpoints[index].Lock, OldIrql);
    }
}

VOID NlTransportSetFormat (_In_ BOOLEAN Capture, _In_ ULONG EndpointIndex,
                           _In_ const WAVEFORMATEX* Format)
{
    auto& endpoint = g_Transport.Endpoints[NlClampEndpoint (EndpointIndex)];
    KIRQL OldIrql;
    KeAcquireSpinLock (&endpoint.Lock, &OldIrql);
    NlSetStreamInfo (Capture ? &endpoint.Info.Capture : &endpoint.Info.Render, Format);
    NlRingReset (Capture ? &endpoint.Capture : &endpoint.Render);
    if (Capture)
    {
        // Recompute the byte target from the live format; the ring was just
        // reset so there is no excess to trim.
        endpoint.Info.CaptureTargetBytes =
            NlCaptureTargetBytes (&endpoint.Info.Capture, endpoint.Info.CaptureTargetMs);
        endpoint.Info.CaptureUsedBytes = 0;
    }
    KeReleaseSpinLock (&endpoint.Lock, OldIrql);
}

ULONG NlTransportPushRender (_In_ ULONG EndpointIndex,
                             _In_reads_bytes_(Bytes) const BYTE* Data, _In_ ULONG Bytes)
{
    if (! g_Transport.Initialized || Data == NULL || Bytes == 0)
        return 0;

    auto& endpoint = g_Transport.Endpoints[NlClampEndpoint (EndpointIndex)];
    KIRQL OldIrql;
    KeAcquireSpinLock (&endpoint.Lock, &OldIrql);
    const ULONG Accepted = min (Bytes, endpoint.Render.Capacity);
    const ULONG RingDropped = NlRingWrite (&endpoint.Render, Data, Accepted);
    endpoint.Info.RenderBytesPushed += Accepted;
    // Bytes the render ring could not keep: the router is not draining
    // READ_RENDER fast enough (oldest audio dropped) or the burst was larger
    // than the whole ring.
    endpoint.Info.RenderOverrunBytes += RingDropped + (Bytes - Accepted);
    if (endpoint.Info.Render.BitsPerSample == 16)
    {
        const auto* Samples = reinterpret_cast<const INT16*>(Data);
        for (ULONG i = 0; i < Accepted / sizeof (INT16); ++i)
        {
            const ULONG Value = (ULONG) (Samples[i] < 0 ? -Samples[i] : Samples[i]);
            if (Value > endpoint.Info.RenderPeak16)
                endpoint.Info.RenderPeak16 = Value;
        }
    }
    KeReleaseSpinLock (&endpoint.Lock, OldIrql);
    return Accepted;
}

VOID NlTransportPeekRender (_In_ ULONG EndpointIndex,
                            _Out_writes_bytes_(Bytes) BYTE* Destination, _In_ ULONG Bytes)
{
    if (! g_Transport.Initialized || Destination == NULL || Bytes == 0)
        return;

    auto& endpoint = g_Transport.Endpoints[NlClampEndpoint (EndpointIndex)];
    KIRQL OldIrql;
    KeAcquireSpinLock (&endpoint.Lock, &OldIrql);
    RtlZeroMemory (Destination, Bytes);
    if (endpoint.Render.Buffer != NULL && endpoint.Render.Capacity > 0 && endpoint.Render.Used > 0)
    {
        const ULONG copy = min (Bytes, endpoint.Render.Used);
        const ULONG first = min (copy, endpoint.Render.Capacity - endpoint.Render.Read);
        RtlCopyMemory (Destination, endpoint.Render.Buffer + endpoint.Render.Read, first);
        if (copy > first)
            RtlCopyMemory (Destination + first, endpoint.Render.Buffer, copy - first);
    }
    KeReleaseSpinLock (&endpoint.Lock, OldIrql);
}

VOID NlTransportPullCapture (_In_ ULONG EndpointIndex,
                             _Out_writes_bytes_(Bytes) BYTE* Destination, _In_ ULONG Bytes)
{
    if (! g_Transport.Initialized || Destination == NULL || Bytes == 0)
        return;

    auto& endpoint = g_Transport.Endpoints[NlClampEndpoint (EndpointIndex)];
    KIRQL OldIrql;
    KeAcquireSpinLock (&endpoint.Lock, &OldIrql);
    RtlZeroMemory (Destination, Bytes);
    const ULONG Pulled = NlRingRead (&endpoint.Capture, Destination, Bytes);
    endpoint.Info.CaptureBytesPulled += Pulled;
    endpoint.Info.CaptureUsedBytes = endpoint.Capture.Used;
    if (Pulled < Bytes)
    {
        // The DMA/timer asked for more audio than the router had queued: the
        // missing tail is the digital silence the capture client will hear.
        endpoint.Info.CaptureUnderrunBytes += Bytes - Pulled;
        endpoint.Info.CaptureUnderrunCalls++;
    }
    KeReleaseSpinLock (&endpoint.Lock, OldIrql);
}

//==============================================================================
static NTSTATUS NlCompleteIrp (_In_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest (Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS NlControlCreate (_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER (DeviceObject);
    UNREFERENCED_PARAMETER (Irp);
    return NlCompleteIrp (Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS NlControlClose (_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER (DeviceObject);
    UNREFERENCED_PARAMETER (Irp);
    return NlCompleteIrp (Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS NlControlDeviceControl (_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER (DeviceObject);

    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation (Irp);
    const ULONG Code = Stack->Parameters.DeviceIoControl.IoControlCode;
    const ULONG InputLength = Stack->Parameters.DeviceIoControl.InputBufferLength;
    const ULONG OutputLength = Stack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    KIRQL OldIrql;

    switch (Code)
    {
    case NLMATRIX_IOCTL_GET_INFO:
        if (Buffer == NULL || OutputLength < sizeof (NLMATRIX_INFO))
            return NlCompleteIrp (Irp, STATUS_BUFFER_TOO_SMALL, 0);

        {
            auto* info = reinterpret_cast<NLMATRIX_INFO*>(Buffer);
            RtlZeroMemory (info, sizeof (*info));
            info->Size = sizeof (NLMATRIX_INFO);
            info->Version = NLMATRIX_ABI_VERSION;
            info->EndpointCount = NLMATRIX_MAX_ENDPOINTS;
            info->RenderFilterCount = g_RenderFilterCount;
            info->CaptureFilterCount = g_CaptureFilterCount;
            for (ULONG index = 0; index < NLMATRIX_MAX_ENDPOINTS; ++index)
            {
                KeAcquireSpinLock (&g_Transport.Endpoints[index].Lock, &OldIrql);
                info->Endpoints[index] = g_Transport.Endpoints[index].Info;
                KeReleaseSpinLock (&g_Transport.Endpoints[index].Lock, OldIrql);
            }
        }
        return NlCompleteIrp (Irp, STATUS_SUCCESS, sizeof (NLMATRIX_INFO));

    case NLMATRIX_IOCTL_READ_RENDER:
    {
        if (Buffer == NULL || OutputLength == 0)
            return NlCompleteIrp (Irp, STATUS_SUCCESS, 0);

        ULONG endpointIndex = 0;
        if (InputLength >= sizeof (NLMATRIX_ENDPOINT_REQUEST))
            endpointIndex = reinterpret_cast<NLMATRIX_ENDPOINT_REQUEST*>(Buffer)->EndpointIndex;
        auto& endpoint = g_Transport.Endpoints[NlClampEndpoint (endpointIndex)];
        const ULONG Bytes = NlRingRead (&endpoint.Render, (BYTE*) Buffer, OutputLength);
        return NlCompleteIrp (Irp, STATUS_SUCCESS, Bytes);
    }

    case NLMATRIX_IOCTL_WRITE_CAPTURE:
    {
        if (Buffer == NULL || InputLength < sizeof (NLMATRIX_WRITE_REQUEST))
            return NlCompleteIrp (Irp, STATUS_INVALID_PARAMETER, 0);

        auto* request = reinterpret_cast<NLMATRIX_WRITE_REQUEST*>(Buffer);
        const ULONG dataBytes = min (request->DataBytes,
                                     InputLength - (ULONG) sizeof (NLMATRIX_WRITE_REQUEST));
        if (dataBytes == 0)
            return NlCompleteIrp (Irp, STATUS_SUCCESS, 0);

        auto& endpoint = g_Transport.Endpoints[NlClampEndpoint (request->EndpointIndex)];
        auto* data = reinterpret_cast<const BYTE*>(request) + sizeof (NLMATRIX_WRITE_REQUEST);
        KeAcquireSpinLock (&endpoint.Lock, &OldIrql);
        const ULONG captureTarget =
            NlCaptureTargetBytes (&endpoint.Info.Capture, endpoint.Info.CaptureTargetMs);
        const ULONG dropped = NlRingWriteBounded (&endpoint.Capture, data, dataBytes, captureTarget);
        endpoint.Info.CaptureUsedBytes = endpoint.Capture.Used;
        endpoint.Info.CaptureTargetBytes = captureTarget;
        endpoint.Info.CaptureDroppedBytes += dropped;
        endpoint.Info.CaptureWriteCalls++;
        KeReleaseSpinLock (&endpoint.Lock, OldIrql);
        return NlCompleteIrp (Irp, STATUS_SUCCESS, dataBytes);
    }

    case NLMATRIX_IOCTL_SET_CAPTURE_TARGET:
    {
        if (Buffer == NULL || InputLength < sizeof (NLMATRIX_CAPTURE_TARGET_REQUEST))
            return NlCompleteIrp (Irp, STATUS_INVALID_PARAMETER, 0);

        auto* request = reinterpret_cast<NLMATRIX_CAPTURE_TARGET_REQUEST*>(Buffer);
        ULONG targetMs = request->CaptureTargetMs;
        if (targetMs < NLMATRIX_CAPTURE_TARGET_MS_MIN)
            targetMs = NLMATRIX_CAPTURE_TARGET_MS_MIN;
        if (targetMs > NLMATRIX_CAPTURE_TARGET_MS_MAX)
            targetMs = NLMATRIX_CAPTURE_TARGET_MS_MAX;

        auto& endpoint = g_Transport.Endpoints[NlClampEndpoint (request->EndpointIndex)];
        KeAcquireSpinLock (&endpoint.Lock, &OldIrql);
        endpoint.Info.CaptureTargetMs = targetMs;
        const ULONG targetBytes = NlCaptureTargetBytes (&endpoint.Info.Capture, targetMs);
        // Reset only the excess already queued above the new target; keep the
        // audio that still fits so a target change does not itself glitch.
        if (targetBytes > 0 && endpoint.Capture.Used > targetBytes)
        {
            endpoint.Info.CaptureDroppedBytes +=
                NlRingDropOldest (&endpoint.Capture, endpoint.Capture.Used - targetBytes);
        }
        endpoint.Info.CaptureTargetBytes = targetBytes;
        endpoint.Info.CaptureUsedBytes = endpoint.Capture.Used;
        KeReleaseSpinLock (&endpoint.Lock, OldIrql);
        return NlCompleteIrp (Irp, STATUS_SUCCESS, 0);
    }

    case NLMATRIX_IOCTL_RESET:
        if (InputLength >= sizeof (NLMATRIX_ENDPOINT_REQUEST))
        {
            const ULONG index = NlClampEndpoint (
                reinterpret_cast<NLMATRIX_ENDPOINT_REQUEST*>(Buffer)->EndpointIndex);
            auto& endpoint = g_Transport.Endpoints[index];
            KeAcquireSpinLock (&endpoint.Lock, &OldIrql);
            NlRingReset (&endpoint.Render);
            NlRingReset (&endpoint.Capture);
            endpoint.Info.CaptureUsedBytes = 0;
            KeReleaseSpinLock (&endpoint.Lock, OldIrql);
        }
        else
            NlTransportReset();
        return NlCompleteIrp (Irp, STATUS_SUCCESS, 0);

    default:
        return NlCompleteIrp (Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static NTSTATUS NlDispatchCreate (_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    if (DeviceObject == g_ControlDevice)
        return NlControlCreate (DeviceObject, Irp);
    return g_OldCreate != NULL ? g_OldCreate (DeviceObject, Irp)
                               : NlCompleteIrp (Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS NlDispatchCleanup (_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    if (DeviceObject == g_ControlDevice)
        return NlCompleteIrp (Irp, STATUS_SUCCESS, 0);
    return g_OldCleanup != NULL ? g_OldCleanup (DeviceObject, Irp)
                                : NlCompleteIrp (Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS NlDispatchClose (_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    if (DeviceObject == g_ControlDevice)
        return NlControlClose (DeviceObject, Irp);
    return g_OldClose != NULL ? g_OldClose (DeviceObject, Irp)
                              : NlCompleteIrp (Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS NlDispatchDeviceControl (_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp)
{
    if (DeviceObject == g_ControlDevice)
        return NlControlDeviceControl (DeviceObject, Irp);
    return g_OldDeviceControl != NULL ? g_OldDeviceControl (DeviceObject, Irp)
                                      : NlCompleteIrp (Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

NTSTATUS NlTransportInitialize (_In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    RtlZeroMemory (&g_Transport, sizeof (g_Transport));
    for (ULONG index = 0; index < NLMATRIX_MAX_ENDPOINTS; ++index)
    {
        auto& endpoint = g_Transport.Endpoints[index];
        KeInitializeSpinLock (&endpoint.Lock);
        endpoint.Info.Size = sizeof (NLMATRIX_ENDPOINT_INFO);
        endpoint.Info.RenderCapacityBytes = NL_RING_CAPACITY;
        endpoint.Info.CaptureCapacityBytes = NL_RING_CAPACITY;
        endpoint.Info.Active = 1;
        NlSetStreamInfo (&endpoint.Info.Render, NULL);
        NlSetStreamInfo (&endpoint.Info.Capture, NULL);
        endpoint.Info.CaptureTargetMs = NL_CAPTURE_TARGET_MS_DEFAULT;
        endpoint.Info.CaptureTargetBytes =
            NlCaptureTargetBytes (&endpoint.Info.Capture, endpoint.Info.CaptureTargetMs);
        NlRingInitialize (&endpoint.Render, NL_RING_CAPACITY);
        NlRingInitialize (&endpoint.Capture, NL_RING_CAPACITY);
        if (endpoint.Render.Buffer == NULL || endpoint.Capture.Buffer == NULL)
        {
            NlTransportCleanup();
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    UNICODE_STRING DeviceName;
    UNICODE_STRING SymbolicLink;
    UNICODE_STRING Sddl;
    RtlInitUnicodeString (&DeviceName, NLMATRIX_DEVICE_NAME);
    RtlInitUnicodeString (&SymbolicLink, NLMATRIX_SYMLINK_NAME);
    RtlInitUnicodeString (&Sddl, L"D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)(A;;GRGWGX;;;WD)(A;;GRGWGX;;;RC)");

    NTSTATUS Status = IoCreateDeviceSecure (
        DriverObject,
        0,
        &DeviceName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &Sddl,
        NULL,
        &g_ControlDevice);
    if (! NT_SUCCESS (Status))
    {
        NlTransportCleanup();
        return Status;
    }

    g_ControlDevice->Flags |= DO_BUFFERED_IO;
    g_ControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    Status = IoCreateSymbolicLink (&SymbolicLink, &DeviceName);
    if (! NT_SUCCESS (Status))
    {
        NlTransportCleanup();
        return Status;
    }

    g_OldCreate = DriverObject->MajorFunction[IRP_MJ_CREATE];
    g_OldClose = DriverObject->MajorFunction[IRP_MJ_CLOSE];
    g_OldCleanup = DriverObject->MajorFunction[IRP_MJ_CLEANUP];
    g_OldDeviceControl = DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL];

    DriverObject->MajorFunction[IRP_MJ_CREATE] = NlDispatchCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = NlDispatchClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = NlDispatchCleanup;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = NlDispatchDeviceControl;

    g_Transport.Initialized = TRUE;
    return STATUS_SUCCESS;
}

VOID NlTransportCleanup()
{
    PAGED_CODE();

    g_Transport.Initialized = FALSE;

    if (g_ControlDevice != NULL)
    {
        UNICODE_STRING SymbolicLink;
        RtlInitUnicodeString (&SymbolicLink, NLMATRIX_SYMLINK_NAME);
        IoDeleteSymbolicLink (&SymbolicLink);
        IoDeleteDevice (g_ControlDevice);
        g_ControlDevice = NULL;
    }

    for (ULONG index = 0; index < NLMATRIX_MAX_ENDPOINTS; ++index)
    {
        auto& endpoint = g_Transport.Endpoints[index];
        if (endpoint.Render.Buffer != NULL)
        {
            ExFreePoolWithTag (endpoint.Render.Buffer, NL_TRANSPORT_POOL_TAG);
            endpoint.Render.Buffer = NULL;
        }
        if (endpoint.Capture.Buffer != NULL)
        {
            ExFreePoolWithTag (endpoint.Capture.Buffer, NL_TRANSPORT_POOL_TAG);
            endpoint.Capture.Buffer = NULL;
        }
    }

    RtlZeroMemory (&g_Transport, sizeof (g_Transport));
}
