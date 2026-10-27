# CLOCK_ISOLATION - layer attribution for the VM half-rate WASAPI clock

Host full power-cycle on 2026-10-27 doubled the guest WASAPI shared-mode
render callback period (20 ms instead of 10 ms). These are FINAL2
router-only probes (no recorder, internal_balanced 1 kHz test tone,
8-10 s). Expected full rate at 960 frames / 48 kHz is ~50 callbacks/s.

| variant | flexasio config | callbacks | uptime s | cb/s | frames | buffer | state | restarts |
|---|---|---|---|---|---|---|---|---|
| a_shared_hd_960 | WASAPI shared, HD Audio, 960 | 506 | 9.902722 | 51.10 | 485760 | 960 | running | 0 |
| b_shared_hd_480 | WASAPI shared, HD Audio, 480 | 1014 | 9.925881 | 102.16 | 486720 | 480 | running | 0 |
| c_shared_nlvirtual_960 | WASAPI shared, NLMatrix virtual render, 960 | 499 | 9.961739 | 50.09 | 479040 | 960 | running | 0 |
| d_shared_cable_960 | WASAPI shared, VB-Cable input, 960 | 499 | 9.958175 | 50.11 | 479040 | 960 | running | 0 |
| e_mme_hd_960 | MME, HD Audio, 960 | 0 | 0.0 | n/a | None | None | None | None | ERROR: Could not open ASIO device: PortAudio host API 'Windows MME' not found
| f_directsound_hd_960 | DirectSound, HD Audio, 960 | 508 | 9.928483 | 51.17 | 487680 | 960 | running | 0 |
| g_exclusive_hd_960 | WASAPI exclusive, HD Audio, 960 | 511 | 9.993097 | 51.13 | 490560 | 960 | running | 0 |

Interpretation: if a/b/c/d are half-rate (~25/s) at 960 frames while
g is real-time (~50/s), the fault is in the guest shared-mode WASAPI
render engine path (device-independent: HD Audio, NLMatrix virtual and
VB-Cable), not in FlexASIO/ASIO/JUCE/the router. Variant b (480
frames) separates buffer size from engine period: ~25/s -> 20 ms engine
period; ~50/s -> buffer/period coupling different. Variants e/f test
non-WASAPI PortAudio backends. Raw logs are in raw/.
