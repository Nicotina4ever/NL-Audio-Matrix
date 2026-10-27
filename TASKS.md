# TASKS — NL Audio Matrix Product Completion

**Version:** 4.1 — completion backlog (engine v2 cycle)
**Updated:** 2026-10-27
**Legend:** `TODO` · `IN PROGRESS` · `BLOCKED` · `TESTING` · `DONE`
**Rule:** a task is DONE only with implementation + integration + a relevant real test. Source compiling is not DONE.
Historical backlog: `docs/TASKS_HISTORY.md`. Engine architecture: `docs/ENGINE_V2.md`.

## A. Audio engine, driver and latency

| ID | Task | Status | Completion criterion / evidence |
|---|---|---|---|
| A1 | True NL Return render→WASAPI loopback pin | DONE (VM) | Independent loopback helper RMS 0.2088 peak 0.3500; reverse router loopback→NL Mic RMS 0.2038 (2026-10-26) |
| A2 | Driver ABI v3: configurable capture target + accurate counters | DONE (VM) | Installed image SHA256 CD95712E...; SET_CAPTURE_TARGET readback 30→50 ms, clamps 10/500 verified; pull/underrun/dropped/render-overrun counters proven with 200 ms injected stall (WAV + counters) |
| A3 | Sample continuity diagnostics | DONE — FINAL4 accepted | Engine A/B on ABI3 driver: underruns 0 / ring drops 0 (engine v2) vs 13–63 per run (legacy). FINAL4 acceptance (frozen AB5E363F, accurate NLMatrix test clock, analyzer 1.2.0 split verdicts): safe x2, balanced, late-consumer, GUI profile = engine/writer PASS + WAV PASS (0 gaps, 0 steady real splices, tone ~1000 Hz); only startup-only kernel underrun deltas 480–12576 B classified WARN. Evidence: `reports/engine/final_acceptance_v5/FINAL_ACCEPTANCE.md`, `reports/engine/final5_markers/`; FINAL4 raw evidence archiviata in `da_eliminare/2026-10-27-consolidation/reports-superseded/` (-FINAL4) |
| A4 | Engine lifecycle detection/recovery | TESTING | Watchdog (2 s stall→ASIO restart state machine; startup false positive fixed), driver/endpoint reconnect with stale-ring drain, no-consumer drain, engine state in METERS. Runtime disconnection exercise on real hardware pending |
| A5 | End-to-end latency measurement | LIMITATO — method validated, definitive hardware number pending | Marker method on FINAL4 + accurate NLMatrix clock + first-callback timestamp recorder: median 38.96–45.46 ms (min 30.06 / max 50.55), ringFill 10–21 ms, driverUsed 50 ms; a ~35–45 ms discrepancy vs the 85–110 ms queue model remains unresolved (WAV→wall timestamp mapping) and is documented. External click path 1484.7 ms is NOT graph latency. Hardware/impulse measurement on a physical path (Focusrite) pending; tooling ready (`vm/engine/latency_focusrite_markers.json`, `tools/measure_engine_latency.py`) |
| A6 | NL Return 1/4 persistent names | BLOCKED | Windows Settings and GUI both show NL Return 1..4; no generic audio registry deletion |
| A7 | Four NL Mic pairs both directions | TESTING | Engine v2: four test tones → NL Mic 1..4, destination underruns/drops 0 (engine side). Reverse NL Return→NL Mic remains open |
| A8 | Bus and mix-minus end-to-end | TODO | REAPER→ReaRoute→bus→NL Mic and mix-minus without feedback; WAV evidence (older partial evidence exists) |
| A9 | Multi-ASIO independent clocks | LIMITATO — documented limitation | Architecture in place (per-device rings/converters, extra-ASIO callbacks). FINAL4 multi_asio: both devices open, no asioError, no hang; master path WAV PASS but d1 ReaRoute starved 1251 because ReaRoute has no client, and d0 shows a 10080 B steady underrun in this scenario. Valid test needs a real ReaRoute client or Focusrite as extra device (blocked by VM USB passthrough). Documented in FINAL_STATUS.md; acceptance raw archiviata in `da_eliminare/2026-10-27-consolidation/reports-superseded/` |
| A10 | Final acceptance (FINAL2→FINAL5) | DONE — FINAL5 accepted and frozen | `tools/vm/Run-FinalAcceptance.ps1` on the accurate NLMatrix clock (analyzer 1.2.0): FINAL3 exposed a balanced servo overrun (10–50 B steady drops + splices) → exact-deficit servo → FINAL4 AB5E363F accepted (safe x2, balanced, GUI, latency, markers PASS; multi-ASIO documented limitation). FINAL5 2AE82FEB = FINAL4 + GUI API (--control graceful stop, devices[] requested-vs-actual, exit 24, driverTargetMs, extra-ASIO buffer, format topology) → frozen and accepted on reports/engine/final_acceptance_v5/ (safe x2, balanced, late-consumer PASS engine+WAV; multi-ASIO documented). GUI: selftest 54/54 (UI dbd69a86...) incl. real 48k→44.1k→48k format change via graceful stop; guiverify 8/8 |
| A11 | VM audio clock isolation/workaround | DONE (isolation matrix) | Layer attribution: the half-rate state was the guest WASAPI **shared-mode** render engine (device-independent; bypassed by exclusive); it disappeared after a later guest boot. Residual WARN was the VMware HD-Audio virtual endpoint clock running ~+2.2% vs guest QPC (shared +2.19%, DirectSound +2.33%, exclusive +2.27%; NLMatrix virtual render +0.18%, VB-Cable +0.22%). Accurate test clock: FlexASIO output to the NLMatrix virtual render endpoint (shared, 960). Matrix: `reports/engine/clock_isolation/CLOCK_ISOLATION.md`. xHCI was also enabled in the VM during Focusrite recovery (vmx changes documented) |

## B. Focusrite and hardware tests

| ID | Task | Status | Completion criterion / evidence |
|---|---|---|---|
| B1 | Focusrite identity/capabilities | DONE | Scarlett Solo USB, Focusrite USB ASIO, 2 in/2 out, 48 kHz observed |
| B2 | Physical input → NL Mic recording | TESTING | Path opened and recorded; real microphone signal captured in the 2026-10-27 session (pre-fix, continuity FAIL) |
| B3 | Real microphone recording (pre-fix reference) | DONE (continuity FAIL) | NL Matrix WAV peak −9.01 dBFS, 21 gaps/327 ms; REAPER direct PCM24 no comparable digital gaps |
| B4 | Post-fix Focusrite recording | BLOCKED (VM USB passthrough) | Extensive recovery attempts on 2026-10-27: user re-plug in a different host port + VM attach, guest device remove/rescan, pnputil restart-device, vmrun reset, full VM power cycle, and finally enabling USB 3.0 xHCI in the vmx. Result is always the same: composite device enumerates (vendor driver 4.150.0, status OK), WASAPI endpoints appear, but streaming fails (capture frames=0, ASIO "Device didn't start correctly"). Conclusion: VMware USB-passthrough limitation for this device/host pair; hardware A/B rows stay prepared for a physical Windows PC test or a repaired VM |
| B5 | Physical monitoring via NL Matrix | TODO | Focusrite input→router→Focusrite output/headphone, distinct from Direct Monitor |
| B6 | Focusrite GUI workflow/naming | TODO | Automatic channel discovery/profile creation |

## C. GUI, mixer and usability

| ID | Task | Status | Completion criterion / evidence |
|---|---|---|---|
| C1 | Left panel accordion/sidebar | TODO | Sections Sources/Destinations/Buses/Mix-minus/Properties/Advanced; collapse/expand; hide sidebar |
| C2 | Automatic device/channel names | TODO | Names such as `Focusrite — Input 1`; custom name preserved |
| C3 | Safe channel picker | TODO | No free numeric crashes; names/directions validated |
| C4 | Magnetic patchbay cables | TODO | Hover highlight, snap, preview, ESC cancel; zoom/pan |
| C5 | Functional mixer strips | DONE (VM, verify screenshots) | Engine v2 GUI fixes: mixer dock min 380 px, strips sized to visible pane, exact 59 px reserved for solo/mute/dB, no bottom clipping at 1920/1600/1366; fader/S/M visible; 30 Hz metering untouched |
| C6 | Layout 1366/1600/1920 + DPI 125/150 | TESTING/DONE-lite | Root causes fixed: sourceEnabled/pan/polarity were not reparented (10 px slivers), bus row overflow, mixer clipping, profile name field. guiverify now asserts layout: **8/8 PASS** at 1900x1020 (all themes/lang), 1600x900 and 1366x768; 20 PNGs under reports/MVP/gui_verify_20261027/. DPI 125/150 still to verify |
| C7 | About/help/microcopy polish | IN PROGRESS | Full links visible; UTF-8 glyphs; no overlap |
| C8 | Italian/English localization | IN PROGRESS (near-complete) | Increments 1–4: centralized L10n + UTF-8 helpers, live switch persisted (`language` in preferences.json), menus/tray (incl. tooltip refresh)/top bar/main panels/Preferences/About/Diagnostics/Mixer/help dialogs/context menus/alerts/~60 statuses/device-browser/process-picker/key tooltips, "(Motore principale)" localized at refreshDevices() creation and selection preserved by item id, theme label localized (Tema: Scuro/Sistema/Chiaro). Remaining (documented): About licence/technical body and VirtualDevices diagnostics dump intentionally English; a handful of technical setStatus(error) lines stay as-is |
| C9 | Theme Dark/System/Light completeness | DONE (VM, verify screenshots) | Fixed real bugs: POLLSYSTEMTHEME never called; secondary windows not using the palette-aware LookAndFeel; TextEditor cached text colour invisible in Light; hardcoded colours -> Palette tokens; mixer fader track contrast in Light (Palette::violet.withAlpha(0.6f)). Verified with 20 PNGs IT/EN × Dark/Light/System x main/Preferences/Diagnostics + layout log 8/8 PASS; deployed UI sha256 313a06b7..., selftest 40/40 |
| C10 | Brand palette/logo integration | IN PROGRESS | Semantic tokens; restrained neon |
| C11 | Buffer modes in GUI + persistence + diagnostics | DONE (VM) | New GUI build selftest 37/37 PASS including new checks: profile engine settings persisted, engine round-trip, preferences.json engine, last-session engine, legacy profile compatibility, balanced default; `preferences_new.json` and `last_session_profile_new.json` verified; diagnostics buffering/health block from METERS |

| C12 | Legibilità tipografica (font/contrasto) | DONE (VM) | User request: caratteri leggermente più visibili. GUI owner: NeonLookAndFeel +1 pt, stronger Palette primary/secondary/disabled tokens, row heights; verify layout at 1366/1600/1920 |
| C13 | Pannello sinistro comprimibile/espandibile | DONE (VM) | User request: lato sorgenti contraibile/espandibile con freccia per espandere il tutto. GUI owner: section disclosure arrows + whole-panel collapse rail, canvas reflow, state persisted in preferences ui object |
| C14 | Sincronizzazione selezione canvas ↔ pannello sinistro | DONE (VM) | User request: selezionando un dispositivo sul canvas i dettagli compaiono nel pannello sinistro e sono modificabili; selezione lista evidenzia il nodo. GUI owner: PatchbayCanvas selection callbacks + left-panel editable details + cable/connection section |
| C15 | Impostazioni reali sample rate / buffer size (GUI + engine API) | DONE (VM, verifica end-to-end) | User request: manca il buffer e la selezione/impostazione di sample rate e buffer size, funzionante e senza glitch. Engine side DONE (dev5 sha 2AE82FEB): --control graceful stop (20 ms fade), devices[] format telemetry, exit 24 on unavailable rate, per-destination driverTargetMs, extra-ASIO buffer. GUI side: probe capabilities, apply via graceful stop+restart, requested-vs-actual warning, persistence, Diagnostics |
| A12 | Graceful stop + device format telemetry (GUI integration API) | DONE — FINAL5 congelato e accettato | dev5 (2AE82FEB, not yet frozen): --control file graceful stop verified exit 0; METERS devices[] requested-vs-actual verified; exit 24 path for unavailable rate (code) ; sameTopology now includes device formats (live change logs restart-required); per-destination driverTargetMs override verified 80 ms; 10 s continuity regression PASS (0 gaps, 0 real splices, steady drop/underrun delta 0, starved 0) |
## D. Build, deployment, platform

| ID | Task | Status | Completion criterion / evidence |
|---|---|---|---|
| D1 | Build ID in GUI/diagnostics | DONE | Title/About show version + compile Build ID; SHA256 recorded |
| D2 | Deploy all components by hash | IN PROGRESS | `tools/Deploy-NLMatrix-ToVM.ps1` verifies GUI/router/bridge/tools/driver package hashes |
| D3 | Driver package version/PnP verification | DONE (VM, ABI3) | Loaded image SHA256/DriverVer match selected package; ABI3 probe OK; rollback package kept |
| D4 | Install/restart/upgrade/uninstall clean | TODO | VM test with defaults preserved |
| D5 | Application/process capture | TODO | Process loopback validated on Windows physical; VM limitation documented |
| D6 | ReaRoute direct/bus/mix-minus/inverse | IN PROGRESS | Direct tone PASS; bus/mix-minus/inverse remain |
| D7 | Release packaging | IN PROGRESS | Portable/developer/repo packages; no public kernel installer |
| D8 | Licence decision/publication | BLOCKED | Owner decision required |
| D9 | Repository cleanup/consolidation (2026-10-27) | DONE | Docs consolidated (README/TASKS/DEVELOPMENT_STATE/TEST_STATUS + technical docs); obsolete moved to `da_eliminare/2026-10-27-consolidation/`; app build objects removed (regenerable); `.gitignore` updated; script/CMake references verified. VM untouched. Delete `da_eliminare` contents only after review (space is not freed by the move). |

## Next active objectives (priority order)

1. **Hardware Focusrite + latency (B4/A5)**: run the prepared legacy-vs-FINAL5
   A/B, real voice recording through NL Mic 1 and the marker latency test on a
   physical Windows PC (VM USB passthrough cannot stream the Scarlett).
2. **Multi-ASIO valido (A9)**: ReaRoute with a live client or Focusrite as extra
   device; verify both clock domains and WAV.
3. **Recovery reale (A4)**: device disconnect/reconnect during routing.
4. **GUI/release**: DPI 125/150 (C6), remaining English technical prose (C8),
   installer/signing/licensing (D4, D7, D8).
5. **Backlog**: C1–C10 remaining, A6–A8, process-loopback on physical Windows.

## Known environment limits (do not re-diagnose blindly)

- VMware USB passthrough for Focusrite: the device enumerates but does not
  stream (ASIO "Device didn't start correctly", WASAPI 0 frame) even after
  re-plug on a different host port, xHCI, guest re-enumeration and VM reset;
  test the hardware path on a physical PC.
- VM guest shared-mode WASAPI render clock can regress to half real-time after a
  host full power cycle (independent of the router).
- Guest timer grid is 15.625 ms; the engine writer uses timeBeginPeriod(1) +
  MMCSS and its servo absorbs the remaining jitter.
- Kernel counter values are cumulative since driver boot; use per-run deltas.