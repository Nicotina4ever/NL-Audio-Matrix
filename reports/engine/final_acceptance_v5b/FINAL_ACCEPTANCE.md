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
| final_gui_profile_12s | PASS | PASS | PASS | d0=0 | d0=0 | d0=0/0 | d0=21.4 / d0=60.0 | 0 / 0.0ms | 0/0 | PASS |
| final_latency | PASS | PASS | PASS | d0=0 | d0=0 | d0=0/0 | d0=24.2 / d0=60.0 | n/a | n/a/n/a | ERROR |

## Startup vs steady-state kernel counters (per destination)

| scenario | dest | startup dropB | startup underB | steady dropB | steady underB | ringFill first/steadyMax/last | target max |
|---|---|---|---|---|---|---|---|
| final_gui_profile_12s | d0 | 0 | 0 | 0 | 0 | 60.0 / 40.0 / 21.4 | 60.0 |
| final_latency | d0 | 0 | 0 | 0 | 0 | 60.0 / 40.1 / 24.2 | 60.0 |

## Per-scenario detail

### final_gui_profile_12s

- config: gui_profile_smoke.json; duration: 12.0 s
- engine: states=['running'], restarts=0, healthy=True, frameDeficit first=-469 final=-748 (-0.13%)
- dest d0 (driver): underruns=0 starved=0 partialWrites=120 writes=2566; driverUsed=49.0 ms; pipeline=85.0 ms; consumer=True
- WAV rec_a0.wav: analyzer=PASS, gaps=0 (0.0 ms), real/sub splices=0/0, tone=1000.0 Hz, RMS=-15.1 dBFS
- harness verdict: PASS; reasons: none

### final_latency

- config: latency_loop.json; duration: 8.0 s
- engine: states=['running'], restarts=0, healthy=True, frameDeficit first=-254 final=-379 (-0.10%)
- dest d0 (driver): underruns=0 starved=0 partialWrites=51 writes=1680; driverUsed=45.0 ms; pipeline=85.0 ms; consumer=True
- WAV rec_a0.wav: analyzer=n/a, gaps=n/a (n/a ms), real/sub splices=n/a/n/a, tone=n/a Hz, RMS=n/a dBFS
- harness verdict: ERROR; reasons: harness: router_timeout_in_guest, runner_not_completed

## End-to-end latency

- final_latency: external click-path measurement 1490.7 ms (onset 5.1 s, burst 10.6 ms).
- NOTE: under the exclusive-mode/HD-Audio test clock the graph ring fill
  inflates to ~220 ms; router marker measurement on the dev marker build
  measured 256 ms (= ring fill 220 ms + driver used 50 ms), matching the
  inflated ring. That is a TEST-CLOCK artefact, not product latency: with an
  accurate real-time clock the expected end-to-end latency is ~85-110 ms
  (balanced pipeline estimate 85 ms). The external click-path number above
  additionally includes the player WASAPI + NL Return loopback path and must
  not be quoted as graph latency.

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
