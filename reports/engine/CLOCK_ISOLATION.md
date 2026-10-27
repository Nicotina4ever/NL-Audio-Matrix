# CLOCK_ISOLATION - layer attribution for the VM audio-clock anomaly

Date: 2026-10-27. Router probe: frozen FINAL2 engine-v2
`vm/baseline/NLMatrixRouter_engine_v2_ACCEPT2.exe` (SHA256
38B4F6C6205165D8615F3B1C2B8C5C61CC151C19FBFBD4A0E5A8420D2E9CBF7D),
router-only 10 s, config `vm/engine/internal_balanced.json` (balanced, internal
1 kHz test tone, no recorder), one FlexASIO.toml variant at a time copied to
`C:\Users\NLMatrix\FlexASIO.toml`. Rate is read from the final METERS
`callbacks`/`uptimeS`; expected full rate for 960 frames at 48 kHz is
50 callbacks/s (100/s at 480 frames). Raw logs: `raw/`, metadata:
`clock_isolation.json`, runner: `tools/vm/Run-ClockIsolation.ps1`.

## Matrix (measured 2026-10-27 17:37-17:39 and re-measured 18:04-18:06)

| variant | FlexASIO config | callbacks | cb/s | frames/s vs 48 kHz | buffer | state | restarts |
|---|---|---|---|---|---|---|---|
| a | WASAPI shared, "Altoparlanti (High Definition Audio Device)", buffer 960 | 506 | 51.10 | +2.19% | 960 | running | 0 |
| b | WASAPI shared, HD Audio, buffer 480 | 1014 | 102.16 | +2.16% | 480 | running | 0 |
| c | WASAPI shared, "Altoparlanti (NLMatrix Virtual Audio Device)", 960 | 499 | 50.09 | +0.18% | 960 | running | 0 |
| d | WASAPI shared, "CABLE Input (VB-Audio Virtual Cable)", 960 | 499 | 50.11 | +0.22% | 960 | running | 0 |
| e | MME, HD Audio, 960 | 0 | n/a | n/a | - | - | - |
| f | DirectSound, HD Audio, 960 | 508 | 51.17 | +2.33% | 960 | running | 0 |
| g | WASAPI exclusive, HD Audio, 960 (control) | 511 | 51.13 | +2.27% | 960 | running | 0 |

e/MME: FlexASIO fails closed with `Could not open ASIO device: PortAudio host API 'Windows MME' not found`
(this FlexASIO/PortAudio build has no MME host API; DirectSound and WASAPI are
available). c/d/devices are reported by name from PortAudio device enumeration.

## Layer attribution

1. **The earlier 2x period anomaly was the guest shared-mode WASAPI render
   engine, not FlexASIO/ASIO/JUCE/router.** On 16:06-16:12 and again after the
   16:19 VM reset (probe at ~16:20) the shared-mode engine delivered 960-frame
   callbacks every ~40 ms (25.3/s, half real-time); the same binary and the
   previous 9B4E1D65 binary both showed it, with no recorder attached, and a
   null-destination run showed it too (Lead). WASAPI **exclusive** bypassed it
   (51.1/s). It was device-independent across WASAPI endpoints we could test.
2. **After the 16:36:45 guest boot the 2x anomaly no longer reproduces.** All
   variants a, b, c, d, f, g now run at real-time callback rates, including
   WASAPI **shared** HD Audio (a: 51.10/s) and shared 480-frame (b: 102.16/s =
   ~10 ms engine period). This was re-measured twice (17:37 and 18:04) on the
   same VM/FINAL2 binary. The exclusive-mode workaround therefore was a
   valid emergency clock at the time but is **not required now**; it is kept
   as a diagnostic control.
3. **The remaining clock defect is a +2.2% HD Audio endpoint sample-clock
   skew vs guest QPC.** HD Audio paths (a shared +2.19%, f DirectSound +2.33%,
   g exclusive +2.27%) deliver ~2.2% more samples per wall second than 48 kHz;
   the virtual/software endpoints are accurate (c NLMatrix +0.18%, d VB-Cable
   +0.22%). This is VMware virtual-HD-Audio device clock skew, not a WASAPI
   period problem: it is present in shared and exclusive mode and explains the
   slow graph-ring fill growth (~22 ms/s) and the resulting
   ring-fill WARNs in the acceptance runs without any engine drop.
4. **Not the ASIO app layer (FlexASIO/PortAudio) or JUCE/router**: every
   backend variant uses the same FlexASIO/PortAudio path and only the device
   clock differs; JUCE/router callback counts follow the driver exactly
   (callbacks = engine callbacks in METERS).

## Conclusions for acceptance

* Half-rate 2x engine period: environment fault, gone since the 16:36:45 boot,
  not a product issue; evidence retained in
  `reports/engine/final_acceptance/blocked_halfrate_5E3BEFED/`.
* Residual clock WARN in acceptance runs: HD Audio +2.2% virtual-device clock
  skew. Virtual endpoints are accurate; a hardware-clock (Focusrite) acceptance
  run is the clean reference when the Scarlett USB path is back.
* MME backend unavailable in this FlexASIO build; DirectSound works and shows
  the same HD-Audio skew.
