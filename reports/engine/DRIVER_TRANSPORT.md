# DRIVER_TRANSPORT — ABI v3 transport instrumentation (kernel)

Status: **DONE — ABI v3 built, installed in the test VM, rebooted and verified**
(2026-10-27; host build 13:38, VM install 14:49, reboot 14:51, verification + smoke 14:52-14:53).
Scope: `driver/sysvad-nl/nltransport.cpp|.h`, `driver/sysvad-nl/nlmatrix_ioctl.h` (+ identical
`driver/nlmatrix_ioctl.h`), `driver/build-probe.ps1`, `driver/test/*`.
No change to the notification/timer mechanism; no allocation added to DPC paths; all new state is
updated under the existing per-endpoint spinlock (IRQL-safe). Host PC was never touched.

## 1. Why (dropout evidence)

The user-mode router writes ~10 ms chunks into the router->capture ring; the capture DMA path
(`CMiniportWaveRTStream::WriteBytes` -> `NlTransportPullCapture`) runs from the 1 ms timer
DPC, asks for the bytes elapsed since the last tick, zero-fills whatever is missing and reads
what is queued. Any router stall longer than the queued target becomes digital silence in the
recorded WAV, but ABI v2 could not prove it:

- `CaptureBytesPulled` was incremented by `dataBytes` in `NLMATRIX_IOCTL_WRITE_CAPTURE`
  (bytes *written*, not pulled) and by the *requested* size in the pull path.
- `CaptureUsedBytes` was only refreshed on writes, never after DMA pulls.
- There was no capture-underrun and no render-overrun counter.
- The capture queue target was a hardcoded 30 ms, not selectable by the router.

## 2. ABI v3 (driver/sysvad-nl/nlmatrix_ioctl.h)

`NLMATRIX_ABI_VERSION = 3`. All v1/v2 IOCTLs keep their codes and semantics; a v2 client
binary must be rebuilt because `NLMATRIX_INFO` grew (GET_INFO rejects too-small output buffers
with `STATUS_BUFFER_TOO_SMALL`; observed in the VM: the old v2 probe got 356 bytes back
while the v3 client expects 436).

Appended to `NLMATRIX_ENDPOINT_INFO` in this exact order (v2 offsets unchanged; `pack(8)`,
all `unsigned long`, endpoint struct now 104 bytes, info blob 436 bytes):

| field | semantics |
|---|---|
| `CaptureTargetMs` | configured per-endpoint capture target in ms, clamped 10..500, default 30 |
| `CaptureWriteCalls` | accepted `WRITE_CAPTURE` calls (data bytes > 0) |
| `CaptureUnderrunBytes` | capture DMA bytes requested while the ring was short (the silence tail) |
| `CaptureUnderrunCalls` | pull calls that underran |
| `RenderOverrunBytes` | render-ring bytes dropped because the router did not drain `READ_RENDER` in time |

Corrected / clarified v2 counters:
- `CaptureBytesPulled` = bytes actually copied out by `NlRingRead` in the DMA pull path.
- `CaptureUsedBytes` = current ring occupancy, refreshed on **both** writes and pulls.
- `CaptureTargetBytes` = byte form of the live target (`ceil(sr*ms/1000)*blockAlign`).
- `CaptureDroppedBytes` = bytes discarded by the target cap (queue beyond target) and by
  `SET_CAPTURE_TARGET` trimming; cumulative.

New IOCTL:

```c
#define NLMATRIX_IOCTL_SET_CAPTURE_TARGET \
    CTL_CODE(NLMATRIX_DEVICE_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _NLMATRIX_CAPTURE_TARGET_REQUEST {
    unsigned long EndpointIndex;
    unsigned long CaptureTargetMs;
} NLMATRIX_CAPTURE_TARGET_REQUEST;
```

Semantics: clamps `CaptureTargetMs` to 10..500, stores it as the endpoint's effective target,
recomputes the byte target from the live capture format, and drops **only the excess** already
queued above the new target (counted in `CaptureDroppedBytes`), preserving the audio that still
fits. No output buffer; read the effective values back with GET_INFO. `RESET` clears the rings
and the `CaptureUsedBytes` gauge but does not zero cumulative byte/call counters.
Clamp/default constants are exported in the header (`NLMATRIX_CAPTURE_TARGET_MS_MIN|MAX|DEFAULT`).

## 3. Kernel implementation (nltransport.cpp)

- `NlRingWrite` returns the number of oldest bytes dropped; render push accumulates
  `RenderOverrunBytes` (ring drop + burst larger than ring); capture write accumulates
  `CaptureDroppedBytes` through `NlRingWriteBounded` and bumps `CaptureWriteCalls`.
- `NlTransportPullCapture` zero-fills, reads what is available, adds only the real bytes to
  `CaptureBytesPulled`, refreshes `CaptureUsedBytes`, and adds the missing tail to
  `CaptureUnderrunBytes`/`CaptureUnderrunCalls`.
- Capture target: `NlCaptureTargetBytes(Info, TargetMs)`; the effective ms lives in
  `Info.CaptureTargetMs` (default 30), recomputed on capture format change and on
  `SET_CAPTURE_TARGET`. The write path no longer reads format state outside the lock.
- Everything runs inside the existing `KeAcquireSpinLock` sections; no allocations were added
  (rings are still allocated once in `NlTransportInitialize`), no change to DPC/timer flow.

## 4. Build (host; no host driver install)

- MSBuild: `D:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe`
  (as hardcoded in `driver/build-driver.ps1`; no fallback needed).
- `driver/build-driver.ps1 -Configuration Release` -> exit 0, DriverVer 10/27/2026 13.38.57.742.
- `driver/package/NLMatrixAudio.sys` size 243712, SHA256
  `CD95712E44AC8319A2864D29EC4CD485DE42A3EA7C6A38FDA022F35566EE02E4`;
  `driver/package/sysvad.cat` SHA256 `1B0B9EA9399766701244FC23A6F5AC5162C9ED448589F027631678E066FB4694`.
- Verification client: `driver/build-probe.ps1` -> `driver/test/bin/NLMatrixTransportProbe.exe`
  (MSVC x64; source `driver/test/nltransport_probe.cpp`). Static asserts pin the ABI layout:
  endpoint info 104 B with offsets Active=80, CaptureTargetMs=84, CaptureWriteCalls=88,
  CaptureUnderrunBytes=92, CaptureUnderrunCalls=96, RenderOverrunBytes=100; info 436 B; request 8 B.
  The old `app/build/NLMatrixDriverTest.exe` is ABI v2 and gets `STATUS_BUFFER_TOO_SMALL`
  from GET_INFO until rebuilt against the new header (router/UI/test tools must be rebuilt together).

## 5. VM install & verification — EVIDENCE

Guest: TESTNLMATRIX, Windows 11 Pro build 26300, test signing already ON (no bcdedit change).
VM install performed by `driver/test/host-vm-abi3.ps1` (host-side orchestration, guest scripts
`guest-install-v3.ps1`, `guest-verify-v3.ps1`, `guest-smoke-v3.ps1`, `guest-idle-check.ps1`).
Evidence directory: `reports/engine/evidence/` (`install-v3.txt`, `verify-v3.txt`,
`smoke-v3.txt`, `idle-v3.txt`, `host-vm-abi3-run.log`, `smoke/`).

### 5.1 Install

- Before: service `sysvad_componentizedaudiosample` RUNNING, image
  `...DriverStore\FileRepository\componentizedaudiosample.inf_amd64_6cffcf41dbe54a52\nlmatrixaudio.sys`,
  SHA256 `709B2BC9...` (ABI v2 known-good).
- `pnputil /add-driver ComponentizedAudioSample.inf /install` -> added **oem18.inf**
  (DriverVer 10/27/2026 13.38.57.742), exit **3010** ("reboot required").
- `devcon update ComponentizedAudioSample.inf Root\sysvad_ComponentizedAudioSample` ->
  "Drivers installed successfully", exit 0.
- Post-install, pre-reboot: service path already points at
  `...componentizedaudiosample.inf_amd64_adba6c20be235103\nlmatrixaudio.sys`, file SHA256
  `CD95712E...`; the running kernel image was still v2 (probe GET_INFO returned 356 bytes
  vs 436 expected) -> REBOOT_REQUIRED.
- Reboot: guest `shutdown /r /t 10 /f` was refused (guest cmd exit 1, boot time unchanged),
  so the host script fell back to `vmrun -T ws reset`; guest came back with
  `LastBootUpTime = 10/27/2026 14:51:04`, Audiosrv Running.
- After reboot: service RUNNING, loaded image SHA256
  `CD95712E44AC8319A2864D29EC4CD485DE42A3EA7C6A38FDA022F35566EE02E4` = built package
  (`DRIVER_HASH_MATCH=YES`), DriverVer 10/27/2026 13.38.57.742, 8 NLMatrix endpoints OK.

### 5.2 ABI v3 and capture-target readback (`NLMatrixTransportProbe.exe`)

```
NLMatrixAudio ABI 3 endpoints=4 filters=4/4 infoBytes=436 endpointBytes=104
RESULT abiVersion=3 expected=3 endpointCount=4 endpointStructBytes=104
ABIV3_OK
```

SET_CAPTURE_TARGET on endpoint 0 (default capture format 48 kHz/2 ch/16 bit, blockAlign 4):

| request ms | readback CaptureTargetMs | CaptureTargetBytes | note |
|---|---|---|---|
| 30 | 30 | 5760 | default |
| 50 | 50 | 9600 | runtime change |
| 5 | 10 | 1920 | clamped to MIN |
| 900 | 500 | 96000 | clamped to MAX |
| 30 | 30 | 5760 | restored |

### 5.3 Capture smoke test (router-sim writer -> NL Mic 1 record)

Guest: `NLMatrixTransportProbe.exe --write` (4 endpoints, 10 ms chunks at 10 ms cadence,
endpoint 0 with an injected **200 ms stall** at mid-run) while
`NLMatrixDriverAudioTest.exe --record --input "NL Mic 1" --seconds 8` captured a WAV.

Counter snapshot, endpoint 0 (mono 48 kHz live capture format => target 30 ms = 2880 B):

```
capture: capBytes=262144 pulled=559488 peak16=0 used=0 targetBytes=2880 targetMs=30 dropped=16512 writeCalls=600
counters: underrunBytes=209664 underrunCalls=290
```

- `writeCalls=600` = 6 s of 10 ms writes; `pulled=559488` = real DMA bytes copied
  (previously this field was polluted by written bytes).
- Monitor time series shows the target tracking ~2880 B and the injected stall:
  t=2.8 s `underrB=114336 underrC=153` -> t=3.1 s `underrB=125088 underrC=168` ->
  t=3.4 s `underrB=130752 underrC=176` (plus the expected target trim
  `dropped=16512` from the burst after the stall). Underruns also appear at startup/tail
  where the recorder runs before/after the writer — expected and now explicit.
- WAV (`reports/engine/evidence/smoke/capture_smoke.wav`, 766124 B, fetched; SHA256
  `5d0aff2e8598f0ad9dba27a66cfbf4e7f555faffbda1716f07c6f4eb58bc32f8`): 383040 frames
  (7.980 s) mono 48 kHz 16-bit, RMS -16.43 dBFS, **dominant frequency 1000.0 Hz** (writer tone) =>
  audio flows end-to-end. Independent gap analysis matches the counters:
  `0.000-1.160 s` startup silence (writer starts later), **`4.180 s +171 ms` = the
  injected 200 ms router stall**, `7.170 s +810 ms` tail after the writer stops; only 12 ms and
  9 ms residual gaps. (Full analyzer output: `.../smoke/analysis.txt`.)

### 5.4 Endpoints, Focusrite USB and idle state

- All 8 endpoints present and OK: **NL Mic 1, NL Mic 2, NL Mic 3, NL Mic 4** + **NL Return 2, NL Return 3**
  + the two render endpoints still named **"Altoparlanti"** and **"SinkDescription Sample"** (render 1
  and 4; pre-existing naming defect documented in `reports/MVP/ENDPOINT_AUDIT.log` — all 4
  render endpoints exist and work, only their friendly names are legacy).
- Focusrite USB: PnP still enumerates "Focusrite USB Audio"
  (`FOCUSRITEUSB\AUDIO&ADAPTER\1&2C3F869&0&`, status OK); raw "Scarlett Solo USB"
  (`USB\VID_1235&PID_8205&MI_00`) status Unknown — interface wedged as reported by the
  Lead, needs a physical replug (no driver action taken).
- VM left idle after the run: "IDLE: no reaper/NLMatrix processes", Audiosrv and
  AudioEndpointBuilder Running, driver service Running, 8 endpoints OK. VM released to vm-harness.

## 6. Rollback

- Known-good package backup (the package installed before this change):
  `driver/package-known-good-20261027-1350/` — sys SHA256
  `709B2BC9D2F6F4BA03526A27D5B600BB9917E65714D1C7462AD4E222EBA2C659` (identical to the old
  DriverStore image used by oem17.inf), plus INF/CAT/CER/devcon; see `BACKUP-NOTE.txt`.
- Rollback on the VM: copy the backup directory to the guest (e.g. `C:\NLMatrix\known-good`),
  then `pnputil /add-driver C:\NLMatrix\known-good\ComponentizedAudioSample.inf /install`
  (and `devcon update ... Root\sysvad_ComponentizedAudioSample` if needed), then reboot the VM.
  Test signing is already ON; no bcdedit change was made. The host PC was never touched.
- ABI v2 works again only with the v2 user-mode clients; v3 client tools fail against the v2 driver
  with `STATUS_BUFFER_TOO_SMALL` (same guard in reverse).

## 7. Remaining risks / follow-ups

- `RenderOverrunBytes` is implemented but **not exercised** by this capture smoke test (no
  render traffic in it): the next pass should play into NL Return with a deliberately slow
  `READ_RENDER` drain and confirm it grows (the field also covers bursts larger than the ring).
- Counters are cumulative ULONGs since driver load; they wrap after 4 GiB and `RESET` does not
  zero them (clients should diff GET_INFO snapshots).
- Underrun bytes also include stream start/stop edges (recorder running before/after the writer) —
  they are evidence, not an error flag; interpret them relative to the write cadence.
- The set capture target is per endpoint pair (router side); Windows' own WaveRT buffering is
  untouched, so the effective end-to-end latency still includes the OS buffer.
- ABI is a hard rebuild boundary: kernel and every user-mode client (router/UI/test tools) must be
  rebuilt together from the v3 header; the frozen pre-fix ABI v2 router binary cannot open the v3
  control device.
- Render endpoint friendly names for NL Return 1/4 are still legacy (pre-existing, unrelated to
  this change); all endpoints are functional.
- Guest reboot after install required a forced `vmrun reset` because the in-guest shutdown
  command was refused (exit 1) — useful to know for future install automation; the fallback in
  `driver/test/host-vm-abi3.ps1` handled it automatically.
