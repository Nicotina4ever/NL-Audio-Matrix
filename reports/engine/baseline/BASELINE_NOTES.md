# Pre-fix baseline run notes (v2 driver, frozen pre-fix engine)

Status: PARTIAL / environment-limited. Do not treat the Focusrite rows as a valid
pre-fix reference; they are recorded as BLOCKED.

## What was run

Harness: tools/vm/Run-ContinuityTest.ps1 + tools/analyze_continuity.py (analyzer
self-test 9/9 PASS). Matrix: tools/vm/Run-BaselineMatrix.ps1, frozen router
binary vm/baseline/NLMatrixRouter_pre_fix.exe
SHA256 e05bbcfa6d9e72ed227833da34d0969c0496ba129736629444dac6e0ba31c9ae
(ABI v2 client, current v2 driver, VM reset to a clean state at 14:13:57 boot).
Matrix started 14:23:54, finished 14:33:33 (435 s), 10 runs.

## Valid rows (FlexASIO master / recorder control)

| scenario | result | d0 underruns | callbacks | framesProcessed | WAV dropouts (steady) | phase jumps/missing | splices |
|---|---|---|---|---|---|---|---|
| a_flex_internal_test_20s_rep1 | FAIL | 9 | 993 | 953280 | rec_a0 11 gaps/97.3 ms/max 20.0; rec_b0 5/20.0/5.0 | a0 0/0; b0 10/36 | 27 / 18 |
| a_flex_internal_test_20s_rep2 | FAIL | 3 | 992 | 952320 | rec_a0 27/295.1/43.0; rec_b0 26/160.0/12.0 | a0 14/25; b0 19/13 | 29 / 51 |
| c_recorder_alone_10s | PASS | - (no router) | - | - | silence control (expected) | - | - |
| d_four_test_tones_15s | FAIL | d0=13 d1=15 d2=14 d3=18 | 741 | 711360 | a0 8/80.2/10.0; a1 9/91.1/11.0; a2 8/80.1/10.0; a3 7/74.0/20.0 (second-half captures much worse: 8-11 gaps, 850-990 ms max) | 0-28 jumps in first half, up to 133 in second half | 4-59 |

Tone analysis of the valid captures: frequencies correct (1000/1040/1080/1120 Hz,
errors < 7 Hz), pre-roll correctly ignored, RMS about -17 dBFS. No held-sample
runs. All FAILs are silent dropouts + phase splices caused by pre-fix writer
starvation under VM scheduling jitter.

## Blocked rows (Focusrite USB ASIO unusable during this baseline)

b_focusrite_in_256_20s_rep1, b_focusrite_in_256_20s_rep2,
b_focusrite_in_512_20s, e_focus_control_noroute_10s,
f_focus_master_internal_test_15s, g_focus_mixed_tone_plus_input_15s.

Failure mode: Focusrite Scarlett Solo enumerates (PnP OK, both WASAPI endpoints
listed) but its USB audio stream is dead:

* router: "Could not open ASIO device: Device didn't start correctly";
  later runs log stops after the Sources line and produce 0 METERS;
* the first Focusrite run delivered 1.05 s of callbacks then froze
  (framesProcessed stuck at 50432, lastCallbackAgeMs grew to 18.7 s);
* WASAPI capture from "Analogue 1 + 2 (Focusrite USB Audio)" returns frames=0;
* VMware log shows USBG "failed to abort pipe usbioerr NOT_BOUND" warnings from
  earlier Focusrite streaming; guest reboot/reset does not clear it.

Recovery needs a physical unplug/re-plug of the Scarlett (or a VMware USB
disconnect/connect). pnputil disable/restart of the USB node is not permitted by
the harness sandbox. Do not use these rows as evidence of pre-fix continuity.

## Environment caveat (important for A/B interpretation)

The pre-fix engine is very sensitive to guest scheduling jitter:

* Windows 11 guest timer resolution is 15.625 ms when no process requests 1 ms
  (Windows per-process timer semantics; verified with NtQueryTimerResolution).
* Measured Sleep(10 ms) wake lateness in the guest: mean 6.1 ms, p95 8.9 ms,
  p99 19.0 ms, max 31.2 ms - the pre-fix writer keeps a fixed 30 ms ring target,
  so occasional late wakeups cause exactly the observed silent gaps.
* The historical pre-fix baseline recorded earlier by the Lead on the same
  binary (0 underruns, clean FlexASIO WAV) was taken in a different host
  scheduling window; it is kept as context only, not as a reproducible number.
* For a fair engine A/B, legacy and engine-v2 matrices must run back-to-back on
  the same VM state / host load. Do not mix numbers from different sessions.

## Artefacts

Each scenario directory under reports/engine/baseline/ contains: full router log
(router.log), runner log, recorder logs, WAVs, analyzer JSON/TXT, run.json
(hashes, params, parsed METERS, verdicts). Aggregated tables:
HARNESS_BASELINE.md (regenerate with tools/vm/make_baseline_report.py).
Diagnostic evidence of the wedged Focusrite probe:
reports/engine/_focuscheck/.

## Reproduce

    pwsh -File "K:\AI\NL Audio Matrix\tools\vm\Run-BaselineMatrix.ps1"

Frozen binaries and hashes: vm/baseline/*.exe + *.sha256.
