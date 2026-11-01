# Third-party virtual audio: static analysis (VB-CABLE, VB-Matrix, VirtualDrivers)

Scope: which third-party virtual endpoints NL Audio Matrix may use as **optional** backends on a stock Windows 11 x64 PC
(Secure Boot on, TESTSIGNING off). Static reads only — nothing installed, registered, extracted-and-run or modified.
Packs unpacked to `%TEMP%`; web content treated as untrusted data.

Sources: `references/third_party/VB-Audio/VBCABLE_Driver_Pack45.zip` (`vbMmeCable64_win10.inf`, `vbaudio_cable64_win10.sys/.cat`,
`VBCABLE_ControlPanel.exe`, `readme.txt`); `references/third_party/VB-Audio/VBAudioMatrix_Setup_v1026.zip` (`VBAudioMatrix_Setup.exe`,
15,864,680 B); GitHub `VirtualDrivers/Virtual-Audio-Driver` tag `25.7.14` (sha256 `dd105609…92bd`).

## 1. VB-CABLE 4.5 — endpoint identity and real capabilities

`Class=MEDIA`, `ClassGUID={4d36e96c-e325-11ce-bfc1-08002be10318}`, `DriverVer = 10/07/2024,3.3.1.7`, `CatalogFile=vbaudio_cable64_win10.cat`,
`PnpLockdown=1` (INF:5-12); HWID `VBAudioVACWDM` (INF:263) decorated `NTamd64.10.0...16299` (INF:32); service `VBAudioVACMME`,
KMDF 1.15, `SERVICE_DEMAND_START` (INF:220-251).

**The INF registers one render + one capture interface pair — not three.** Render: `KSCATEGORY_AUDIO`+`KSCATEGORY_REALTIME`+
`KSCATEGORY_RENDER` on `WaveRender1`, `+KSCATEGORY_TOPOLOGY` on `TopoRender1` (INF:166-170). Capture: `KSCATEGORY_AUDIO`+
`KSCATEGORY_REALTIME`+`KSCATEGORY_CAPTURE` on `WaveCapture1`, topology pair on `TopoCapture1` (INF:176-180). Interface names
`VBAudioVACWDM2022_Out1_Wave_E51A78A01FB100001` / `…_In1_Wave_…F1B200001` (INF:278-284, repeated in the `.sys`).

**Three display names exist, but only as media-category keys** (INF:308-316, 70-77): `CABLE Output` (capture, `{B961F7FC-…B74}`),
`CABLE Input` (render, `{B961F7FD-…B74}`), `CABLE In 16 Ch` (render, `{B961F7FD-…B75}`). Same three strings in
`VBCABLE_ControlPanel.exe` (`CABLE Input`, `CABLE In 16ch`, `CABLE Output`; note `16ch` vs INF `16 Ch`). The `.sys` has **no**
`CABLE*` strings — names come from INF/control panel only.

**Channels (decisive).** `KSDATARANGE_AUDIO` blocks (`KSDATAFORMAT_TYPE_AUDIO`+`SUBTYPE_PCM`+`SPECIFIER_WAVEFORMATEX`) with tail
`MaximumChannels=16, bits 16..24, 11025..192000 Hz` sit at `.sys` offsets `0xC470`,`0xC4C8` and `0xFF60`,`0xFFB8`; the entire pin-format
table appears **twice** (`0x9670…0xC388` and `0xD160…0xFE78`) with one 16-ch range pair each ⇒ **render and capture pins have identical
16-channel capacity, so `CABLE Output` carries up to 16 ch**. Enumerated: 224 `KSDATAFORMAT_WAVEFORMATEXTENSIBLE` entries, channels
**{2,4,6,8}**, rates **{11025,12000,16000,22050,24000,32000,44100,48000,64000,88200,96000,128000,176400,192000}**, bits **{16,24}**;
`IEEE_FLOAT` subtype occurrences **0** ⇒ **integer PCM only, no float anywhere.** **10 channels are not a supported count** — use **8**
(enumerated, selectable in Sound→Advanced) or **16** (range-only, KS/WASAPI exclusive).

**Default format = stereo.** `PKEY_AudioEngine_OEMFormat` for `EP\0`/`EP\1` decodes to `0xFFFE, ch=2, 48000, block 6, 24 bit, mask 3`
(INF:215-216) ⇒ shared-mode endpoints present as **2-channel**; multichannel needs a 4/6/8-ch default chosen in Sound→Advanced, or
exclusive/KS with a custom `WAVEFORMATEXTENSIBLE`.

**Loopback semantics** — `readme.txt`: "All signals sent to the device output is going on the device input." ⇒ audio written to render
`CABLE Input` is captured at `CABLE Output`; one direction per cable.

**Latency/clock** (`VBCABLE_ControlPanel.exe` strings, service key `…\Services\VB-Cable`): internal SR 44100…192000; presets
`3x 128`…`3x 8192` SMP with **default `7168 smp` (≈149 ms @48 kHz)**; max latency ≤32768 SMP ("requires REBOOT"); flags
`VBAudioCableWDM_LoopBack` ("Enable Loopback Streaming"), `VBAudioCableWDM_VolCtl`, `VBAudioCableWDM_SR`.

**Signing (verified locally).** `.cat`: `CN=Microsoft Windows Hardware Compatibility Publisher`, issuer `Microsoft Windows Third Party
Component CA 2014`, Valid ⇒ WHQL/attestation ⇒ **loads on stock Win11**. `.sys` separately EV-signed `CN=BUREL VINCENT Entrepreneur
individuel`, issuer `GlobalSign GCC R45 EV CodeSigning CA 2020`, 1 `WIN_CERTIFICATE` (type 0x0002).

**Licence.** `readme.txt`: donationware; as-is copy/diffusion allowed; "It is not allowed to integrate the VB-CABLE package in another
software installation procedure without Author agreement" ⇒ **must not be bundled**; fine as a user-installed optional dependency.

**Unproven:** whether `CABLE In 16 Ch` is a concurrently instantiated second render endpoint (INF registers one render interface; the
duplicate 16-ch pair is at pin level); channel count shown per named endpoint; coexistence of a second device instance (interface
reference strings are hard-coded `Out1`/`In1` ⇒ duplicate names; the vendor's A+B/C+D packs exist for this reason).

## 2. VB-Matrix 1.0.2.6 — what a licensed user already has

Custom installer, **no** ZIP (`PK\x03\x04`=0), **no** 7z/CAB/LZMA, **no** NSIS/Inno/InstallShield marker; PE overlay ≈13 KB ⇒ payload
packed in the PE. INF text is not in clear (`AddInterface`, `KSCATEGORY_AUDIO`, `Class=MEDIA`, `AddService`, `HKR,` = 0 hits).

**Installed-file manifest**, ASCII table `0x144830–0x145550`: `readme.txt`, `VBAudioMatrix.exe`, `VBAudioMatrix_x64.exe`,
`VBANScreen.exe`, `vbmatrixvaio64_win10.cat/.inf/.sys`, `vbmatrixvaio64arm_win10.sys`, `VBMatrixVAIO_ControlPanel.exe`,
`VBMatrixVAIO_Setup_x64.exe`, `vbmatrix_vasio8_driver(.64).dll`, `vbmatrix_vasio64A_driver(.64).dll`,
`vbmatrix_vasio64B_driver(.64).dll`, `vbmatrix_vasio128_driver(.64).dll`, `vbregsvr64.exe`, app PNGs.
⇒ one VAIO WDM package (x64+ARM64, one catalog), VAIO control panel, driver setup helper, Matrix router (x86+x64), COM helper, and
**four virtual ASIO drivers with 8, 64, 64 and 128 channels**, each 32- and 64-bit (counts encoded in the DLL names — the only static proof).

**Endpoint creation mechanism:** UTF-16 `VBMatrix` / `VBMatrixVAIO` (0x274C8/0x27968) and base names **`VBMatrix In`** (0x27984),
**`VBMatrix Out`** (0x2799C) ⇒ render "VBMatrix In <n>", capture "VBMatrix Out <n>", count/names managed at runtime by
`VBMatrixVAIO_ControlPanel.exe` via the `vbmatrixvaio64_win10` service. Endpoint count is a runtime setting, not an INF constant.
**Licence:** not in clear text (only a UI-level `License` hit at 0x2B744; `Burel`/`Copyright`/`LICENSE`/`donationware`/`Trial` = 0; the
packed `readme.txt` is unreadable) ⇒ **unproven**; assume paid, no redistribution, no bundling.
**Also unproven:** per-endpoint channel counts, endpoint maximum, whether the ASIO drivers work with the Matrix app closed.
*Relevance:* a VB-Matrix owner can already offer NL Matrix a 128-ch ASIO endpoint plus named render/capture endpoints and an internal
router — a diagnostic/optional route and a direct competitor for the same job, never a dependency.

## 3. VirtualDrivers 25.7.14 (GitHub, "Virtual Audio Driver by MikeTheTech")

**Signature — confirmed, and it is the blocker.** `VirtualAudioDriver.sys` security directory at file offset `0xF400`, size 12,088 ⇒
**exactly 1 `WIN_CERTIFICATE`** (type 0x0002 PKCS#7, rev 0x0200, `DigiCert Timestamp 2024`), signer `CN=SignPath Foundation`, issuer
`GlobalSign GCC R45 CodeSigning CA 2020`. `virtualaudiodriver.cat` signed by the same SignPath cert, and a byte scan of the catalog
finds **zero occurrences of "Microsoft"** ⇒ **no Microsoft WHQL/attestation catalog**. A code-signing cert cannot authorize a
kernel-mode driver on Win11 x64 under Secure Boot ⇒ **expected `Code 52`; the device will not start.** Upstream README says it:
*"This software is in beta and requires test signing to be enabled"* + `bcdedit /set testsigning on`. **No workaround exists without
TESTSIGNING** short of a fresh HLK/attestation submission (Microsoft partner EV cert) or our own Microsoft-signed package.

**User-mode injection interface: none.** SysVAD-derived PortCls/WaveRT miniport. `Source/Inc/endpoints.h` declares only standard KS
pins/nodes (`KSPIN_WAVE_RENDER2_SINK_SYSTEM`/`_LOOPBACK`/`_SOURCE`, `KSPIN_WAVEIN_HOST`, `KSNODE_TOPO_*`); the only custom constants are
product GUIDs (`PID_VIRTUALAUDIODRIVER`, `NAME_SIMPLE_AUDIO_SAMPLE`). `Source/Main/minwavert.h`: the stream class holds only the WaveRT
cyclic buffer and standard property handlers — **no IOCTL, no device-interface GUID, no shared-memory ring.** README confirms the
intent: *"…named pipes, shared memory buffers (for no-latency audio), direct integration… Contact us for quotes on a custom build!"*
⇒ the signed build **does not accept injected PCM**.

**The capture endpoint is an independent fake device.** `Source/Main/VirtualAudioDriver.inx` registers separate `WaveSpeaker`+
`TopologySpeaker` (`KSCATEGORY_RENDER`) and `WaveMicArray1`+`TopologyMicArray1` (`KSCATEGORY_CAPTURE`) sets with no bridge between them;
the capture filter's pins are only `KSPIN_WAVE_BRIDGE`+`KSPIN_WAVEIN_HOST`. Nothing carries the speaker stream into the mic ⇒ "virtual
microphone" is a silent independent device, not a monitor. (The render endpoint *is* WASAPI-loopback-capturable — ordinary Windows
mechanism, no driver support needed.)

**Multiple instances.** HWID `ROOT\VirtualAudioDriver` is root-enumerable ("Add Legacy Hardware"/`pnputil`), so N devnodes *can* be
created from the same signed INF; coexistence of N instances is **unproven** (static topology GUIDs/names, no instance index in source).
Moot — the package cannot load. Note also `[AUDIOHW.AddReg]` sets `DeviceType=0x1D` and a permissive SDDL
`D:P(A;;GA;;;SY)(;;GRGWGX;;;BA)(;;GRGWGX;;;WD)(;;GRGWGX;;;RC)` (any user may open the KS device), but the only protocol there is
standard KS/WASAPI audio, not an injection API.

## 4. Comparative verdict

| | Stock Win11 (no TESTSIGNING) | Endpoints / channels | NL Matrix send **and** receive | Latency / format | Licence / distribution | Gain for "few endpoints, many channels" |
|---|---|---|---|---|---|---|
| **VB-CABLE 4.5** | **YES** — WHQL catalog + EV `.sys` | 1 cable; `CABLE Input`, `CABLE In 16 Ch`, `CABLE Output`; ranges **≤16 ch**; enumerated 2/4/6/8; **no float**; 11.025–192 kHz; 16/24-bit | **Both** — write `CABLE Input`, read `CABLE Output` (same cable ⇒ legs not independent) | Default **7168 smp ≈149 ms**; presets 3×128…3×8192; SR 44.1–192 k | Donationware; as-is copy OK; **bundling forbidden** | High but fixed: 1 device, fixed names, no 10-ch, no per-endpoint config |
| **VB-Matrix 1.0.2.6** | YES (paid; VAIO + 128-ch ASIO) | Configurable `VBMatrix In/Out <n>` + ASIO 8/64/64/128 ch | Both, via its own router | Its own ASIO latency; router must run | **Unproven** (packed); assume paid, no redistribution | Highest capability, but duplicates NL Matrix's job; not a neutral cable |
| **VirtualDrivers 25.7.14** | **NO — Code 52** (SignPath only, no MS catalog) | 1 render + 1 capture; 2-ch fake mic | **Receive only** (WASAPI loopback of its speaker); **cannot feed** the mic | N/A (cannot load) | MIT/GitHub, but unusable kernel payload | None |
| *ReaRoute (reference)* | ASIO driver, not a Windows endpoint; needs REAPER running | ASIO channels only, no WDM endpoints | Via ASIO only | Depends on REAPER block size | Ships with REAPER; not redistributable | Low for Windows-endpoint goals *(not analysed here)* |
| *Real hardware loopback (reference)* | YES, by definition | whatever the interface exposes | Yes, if the device has loopback | Hardware clock | Owned hardware | No virtual endpoints at all |

*(ReaRoute / hardware rows are reference only — not statically analysed here; treat as unverified.)*

**Decisive facts:** (a) only VB-CABLE has a **Microsoft WHQL catalog** — the sole property that lets a third-party WDM audio driver load
on a stock Win11 box; (b) VB-CABLE pins carry **16-ch data ranges** but **no float and no 10-ch** format; (c) VirtualDrivers has **one
SignPath signature and no Microsoft catalog** ⇒ Code 52, and its mic has **no user-mode injection path**; (d) VB-CABLE is
**single-cable and non-bundlable**.

## 5. What NL Matrix can and cannot achieve through these signed endpoints

**Can — output side:** play into a VB-CABLE **render** endpoint and let apps record its **capture** endpoint; ≤**16 ch** per cable
(8 is the practical enumerated count), 16/24-bit integer, 11.025–192 kHz. **Can — input side:** capture that cable's **capture**
endpoint (the app plays into its render side), or use ordinary **WASAPI loopback** on any render device — no third-party driver
needed for the loopback route. **Can:** isolate one app per cable (the real reason to keep VB-CABLE as a diagnostic path).

**Cannot:** present 10 channels (unsupported count — use 8 or 16); use IEEE-float anywhere on the cable; make the stereo-default
endpoint expose N channels without a Sound→Advanced change or exclusive/KS mode; get an arbitrary *number* of endpoints or arbitrary
names (one cable per install, hard-coded interface names, bundling forbidden); get low latency by default (≈149 ms until lowered to
3×128 ≈8 ms + reboot); escape the Windows audio engine's clock/resampling — the cable is a device, not a shared-memory transport.

**Minimum endpoint set for a 10-channel Apollo workflow** (Apollo already talks ASIO/WASAPI directly; the cable carries *other* apps' audio):
1. **Output leg** — 1 × 16-ch cable: NL Matrix renders 10 of 16 channels into `CABLE Input`; the remote/chat app records `CABLE Output`
   (2 ch if stereo — it just takes L/R).
2. **Input leg** — 1 × separate cable: the app under test plays into that cable's render side; NL Matrix captures its capture side
   *(only if per-app isolation is required; otherwise WASAPI loopback on the real device suffices).*
⇒ **2 cable instances** for independent in/out. A single VB-CABLE install gives **one** cable, so the legs cannot be independent —
exactly why VB-Audio sells multi-cable packs and why NL Matrix ultimately needs its own driver.

**Lost without our own driver:** provisionable N×M endpoints with programmable names and per-endpoint channel counts; float and
arbitrary counts (10); a low-latency clock-slaved user/kernel transport instead of WASAPI; per-endpoint independence; freedom from a
third party's licence, WHQL re-certification and install constraints.

## 6. Should VirtualDrivers be discarded?

**Yes — discard it.** It carries a single SignPath code-signing signature and no Microsoft catalog, so on a stock Windows 11 x64 machine
it loads only with TESTSIGNING (Code 52 otherwise), and even then its capture endpoint is an independent fake device with no
IOCTL/shared-memory path to inject PCM (README sells that capability as a paid custom build).

*Every `unproven` item is labelled inline. Nothing was executed, installed or registered; VB-Matrix's licence text and endpoint count
are the only questions left genuinely open by static analysis.*
