# FINAL ACCEPTANCE - engine continuity suite on ABI3 driver (split verdicts)

Split verdicts (honest): **engine/writer**, **WAV end-to-end continuity**,
**clock/environment**. The clock/environment column is WARN when the test
clock itself (VMware HD-Audio skew / exclusive-mode startup burst) explains
ring-fill drift, a large negative frameDeficit or startup-only counter deltas;
it is not an engine drop. Startup transients are classified separately.

Router binaries: NLMatrixRouter.exe = 2ae82febef4df4bb7de9f7d2c149f2a28202cc7f4c73a7ec2ec58d43eeed0b86

## Verdict summary

| scenario | engine/writer | WAV continuity | clock/env | underruns | starved | steady dropB/underB | ringFill last / targetMax | gaps | splices real/sub | harness |
|---|---|---|---|---|---|---|---|---|---|---|
| final_internal_balanced_rep1 | PASS | PASS | WARN | d0=0 | d0=0 | d0=0/0 | d0=15.4 / d0=60.0 | 0 / 0.0ms | 0/0 | PASS |
| final_internal_safe_rep1 | PASS | PASS | WARN | d0=0 | d0=0 | d0=0/0 | d0=59.0 / d0=150.0 | 0 / 0.0ms | 0/0 | PASS |
| final_internal_safe_rep2 | PASS | PASS | WARN | d0=0 | d0=0 | d0=0/0 | d0=69.6 / d0=150.0 | 0 / 0.0ms | 0/0 | PASS |
| final_late_consumer_15s | PASS | PASS | PASS | d0=0 | d0=0 | d0=0/0 | d0=28.1 / d0=60.0 | 0 / 0.0ms | 0/0 | PASS |
| final_multi_asio_15s | FAIL | PASS | WARN | d0=0,d1=0 | d0=0,d1=1320 | d0=0/0,d1=0/0 | d0=0.4,d1=0.0 / d0=60.0,d1=60.0 | 0 / 0.0ms | 0/2 | FAIL |

## Startup vs steady-state kernel counters (per destination)

| scenario | dest | startup dropB | startup underB | steady dropB | steady underB | ringFill first/steadyMax/last | target max |
|---|---|---|---|---|---|---|---|
| final_internal_balanced_rep1 | d0 | 0 | 1440 | 0 | 0 | 40.0 / 30.1 / 15.4 | 60.0 |
| final_internal_safe_rep1 | d0 | 0 | 10656 | 0 | 0 | 60.0 / 80.9 / 59.0 | 150.0 |
| final_internal_safe_rep2 | d0 | 0 | 10656 | 0 | 0 | 60.0 / 81.0 / 69.6 | 150.0 |
| final_late_consumer_15s | d0 | 0 | 0 | 0 | 0 | 55.0 / 80.0 / 28.1 | 60.0 |
| final_multi_asio_15s | d0 | 0 | 96 | 0 | 0 | 55.0 / 30.9 / 0.4 | 60.0 |
| final_multi_asio_15s | d1 | 0 | 0 | 0 | 0 | 0.0 / 0.0 / 0.0 | 60.0 |

## Per-scenario detail

### final_internal_balanced_rep1

- config: internal_balanced.json; duration: 20.0 s
- engine: states=['running'], restarts=0, healthy=True, frameDeficit first=498 final=-15 (-0.00%)
- dest d0 (driver): underruns=0 starved=0 partialWrites=92 writes=4217; driverUsed=49.0 ms; pipeline=85.0 ms; consumer=True
- WAV rec_a0.wav: analyzer=PASS, gaps=0 (0.0 ms), real/sub splices=0/0, tone=1000.0 Hz, RMS=-17.0 dBFS
- clock/environment notes: d0 startup kernel counter delta 1440 B (t<1 s)
- harness verdict: PASS; reasons: none

### final_internal_safe_rep1

- config: engine_internal_safe.json; duration: 20.0 s
- engine: states=['running'], restarts=0, healthy=True, frameDeficit first=-466 final=-116 (-0.01%)
- dest d0 (driver): underruns=0 starved=0 partialWrites=0 writes=2132; driverUsed=88.0 ms; pipeline=175.0 ms; consumer=True
- WAV rec_a0.wav: analyzer=PASS, gaps=0 (0.0 ms), real/sub splices=0/0, tone=1000.1 Hz, RMS=-17.0 dBFS
- clock/environment notes: d0 startup kernel counter delta 10656 B (t<1 s)
- harness verdict: PASS; reasons: none

### final_internal_safe_rep2

- config: engine_internal_safe.json; duration: 20.0 s
- engine: states=['running'], restarts=0, healthy=True, frameDeficit first=-469 final=-321 (-0.03%)
- dest d0 (driver): underruns=0 starved=0 partialWrites=0 writes=2276; driverUsed=100.0 ms; pipeline=175.0 ms; consumer=True
- WAV rec_a0.wav: analyzer=PASS, gaps=0 (0.0 ms), real/sub splices=0/0, tone=1000.1 Hz, RMS=-17.0 dBFS
- clock/environment notes: d0 startup kernel counter delta 10656 B (t<1 s)
- harness verdict: PASS; reasons: none

### final_late_consumer_15s

- config: late_consumer.json; duration: 15.0 s
- engine: states=['running'], restarts=0, healthy=True, frameDeficit first=-397 final=-292 (-0.04%)
- dest d0 (driver): underruns=0 starved=0 partialWrites=0 writes=2024; driverUsed=49.0 ms; pipeline=85.0 ms; consumer=True
- WAV rec_a0.wav: analyzer=PASS, gaps=0 (0.0 ms), real/sub splices=0/0, tone=1000.2 Hz, RMS=-17.0 dBFS
- harness verdict: PASS; reasons: none

### final_multi_asio_15s

- config: multi_asio.json; duration: 15.0 s
- engine: states=['running'], restarts=0, healthy=True, frameDeficit first=-382 final=441 (+0.06%)
- dest d0 (driver): underruns=0 starved=0 partialWrites=22 writes=3191; driverUsed=39.0 ms; pipeline=85.0 ms; consumer=True
- dest d1 (asio-out): underruns=0 starved=1320 partialWrites=0 writes=0; driverUsed=-1.0 ms; pipeline=85.0 ms; consumer=False
- WAV rec_a0.wav: analyzer=PASS, gaps=0 (0.0 ms), real/sub splices=0/2, tone=1000.0 Hz, RMS=-17.0 dBFS
- engine/writer failure reasons: d1 starved=1320
- clock/environment notes: d0 startup kernel counter delta 96 B (t<1 s)
- harness verdict: FAIL; reasons: writer_starved_d1=1320

## Notes

* Split verdicts: a harness FAIL caused only by clock/environment counters does
  not turn engine/writer or WAV continuity into a FAIL; both are reported.
* Startup deltas are taken from the first METERS sample to the first sample with
  uptime >= 1 s; steady deltas from there to the last sample.
* multi_asio extra-ASIO output row is reported as its own destination (d1).
* UI row: uses the OLD deployed GUI build for this run (2 known selftest
  failures: canvas interaction nodes missing, GUI parses actual router meter
  values; last-session profile has no "engine" object). A dedicated GUI
  acceptance run with the fixed build is pending.
* Reproduction: pwsh -File tools\vm\Run-FinalAcceptance.ps1 -RouterExe <FINAL2> -ClickWav <click_3s.wav> -LatencyConfig <latency_loop.json> -GuiConfig <gui_profile_smoke.json>
* Engine-side acceptance remains the 9B4E1D65 A/B evidence; FINAL2 delta was
  verified semantically (no-consumer drain, reports/engine/final_acceptance/
  blocked_halfrate_5E3BEFED/FINAL2_delta_semantic_test.md and CLOCK_ISOLATION.md).
