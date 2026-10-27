#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
analyze_continuity.py - rigorous audio continuity analyzer for the NL Audio Matrix
dropout/continuity test harness.

Purpose
-------
Given a WAV captured from a driver capture endpoint (mono PCM16 from
NLMatrixDriverAudioTest.exe, but any PCM WAV works), decide PASS/FAIL for
continuity and quantify every defect:

  * signal onset detection and pre-roll measurement (the recorder starts
    before the router, so the first ~2-3 s are silence and MUST be ignored);
  * zero-run ("silent dropout") detection after onset, with startup-gap
    classification (a gap right at onset is a start transient, reported
    separately from steady-state dropouts);
  * frozen/held-sample detection (a buffer repeated instead of new audio);
  * adjacent-sample delta / click statistics;
  * tone estimation (zero crossings, FFT, and an exact second-order recurrence
    fit) with frequency error, SNR, phase drift in ppm, phase-jump events and
    the number of skipped/duplicated frames implied by them;
  * recurrence-residual boundary detection: for a known pure tone, any splice,
    skipped sample or inserted block violates y[n] = 2cos(w)y[n-1] - y[n-2];
    the residual pinpoint the exact boundary sample.

The analyzer is deterministic: same WAV + same options => byte-identical JSON
(no timestamps, no randomness, stable ordering, rounded floats).

1.2.0 startup-window classification
-----------------------------------
Every event type (silent gap, frozen/held run, phase jump, real splice) that
starts within --startup-window-ms (default 50 ms) after the detected onset is a
STARTUP TRANSIENT: kept visible with timestamps in the JSON details and
reported as warnings, but not a FAIL reason. PASS requires zero STEADY-STATE
events of every type plus clean tone/level/capture checks.

Verdict semantics
-----------------
  PASS  no steady-state silent gaps, held runs, phase jumps or real splices;
        tone frequency/level within tolerance, no clipping, capture length
        plausible.
  FAIL  otherwise; "reasons" lists every failed steady-state criterion.

Exit codes: 0 = PASS, 1 = FAIL, 2 = analyzer/harness error.

Usage
-----
  python analyze_continuity.py capture.wav --tone 1000 --tone-level 0.25 \
      --record-window 10 --json out.json --label scenario-name
  python analyze_continuity.py capture.wav --expect-silence --json out.json
  python analyze_continuity.py capture.wav --expect-signal --json out.json
"""
import argparse
import hashlib
import json
import math
import os
import sys
import wave

import numpy as np

ANALYZER_NAME = "analyze_continuity.py"
ANALYZER_VERSION = "1.2.0"
TWO_PI = 2.0 * math.pi


# --------------------------------------------------------------------------- #
# helpers
# --------------------------------------------------------------------------- #
def rnd(x, nd=6):
    """Deterministic float rounding; keeps JSON compact and byte-stable."""
    try:
        v = float(x)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(v):
        return None
    if v == 0.0:
        return 0.0
    return round(v, nd)


def dbfs(x):
    x = float(x)
    if x <= 0.0 or not math.isfinite(x):
        return -240.0
    return 20.0 * math.log10(x)


def rms(x):
    if len(x) == 0:
        return 0.0
    return float(np.sqrt(np.mean(np.square(x, dtype=np.float64))))


def wrap_angle(a):
    """Wrap angle(s) to (-pi, pi]."""
    return (np.asarray(a) + math.pi) % TWO_PI - math.pi


def load_wav(path):
    """Return (data float64 [frames, channels], sample_rate, bits, format_name)."""
    with wave.open(path, "rb") as w:
        nframes = w.getnframes()
        nch = w.getnchannels()
        sw = w.getsampwidth()
        sr = w.getframerate()
        comptype = w.getcomptype()
        raw = w.readframes(nframes)
    if comptype != "NONE":
        raise ValueError("compressed WAV not supported: %s" % comptype)
    if sw == 1:
        x = np.frombuffer(raw, dtype=np.uint8).astype(np.float64) / 128.0 - 1.0
        bits = 8
    elif sw == 2:
        x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
        bits = 16
    elif sw == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        v = (b[:, 0].astype(np.int32)
             | (b[:, 1].astype(np.int32) << 8)
             | (b[:, 2].astype(np.int32) << 16))
        v = np.where(v >= (1 << 23), v - (1 << 24), v)
        x = v.astype(np.float64) / float(1 << 23)
        bits = 24
    elif sw == 4:
        x = np.frombuffer(raw, dtype="<i4").astype(np.float64) / float(1 << 31)
        bits = 32
    elif sw == 8:
        x = np.frombuffer(raw, dtype="<f8").astype(np.float64)
        bits = 64
    else:
        raise ValueError("unsupported sample width %d" % sw)
    if nch > 1:
        x = x.reshape(-1, nch)
    else:
        x = x.reshape(-1, 1)
    return x, sr, bits, "PCM"


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def runs_of_mask(mask):
    """Return list of (start, end) half-open runs where mask is True."""
    mask = np.asarray(mask, dtype=bool)
    if mask.size == 0:
        return []
    d = np.diff(mask.astype(np.int8))
    starts = np.where(d == 1)[0] + 1
    ends = np.where(d == -1)[0] + 1
    if mask[0]:
        starts = np.r_[0, starts]
    if mask[-1]:
        ends = np.r_[ends, mask.size]
    return [(int(a), int(b)) for a, b in zip(starts, ends)]


# --------------------------------------------------------------------------- #
# onset detection
# --------------------------------------------------------------------------- #
def detect_onset(y, sr):
    """Detect the first sample of sustained signal.

    Returns dict: detected, onset_sample, preroll_s, noise_rms, signal_rms.
    Strategy: 10 ms RMS windows with 5 ms hop; noise floor from the 10th
    percentile of window RMS; threshold = max(4*noise, -60 dBFS RMS); first
    window whose RMS exceeds the threshold and stays above it for >= 30 ms.
    If the file already starts loud, onset is 0.
    """
    n = len(y)
    hop = max(1, int(round(0.005 * sr)))
    win = max(hop, int(round(0.010 * sr)))
    if n < win:
        r = rms(y)
        return {"detected": r > 1e-3, "onset_sample": 0, "preroll_s": 0.0,
                "noise_rms": rnd(r), "signal_rms": rnd(r)}
    nwin = 1 + (n - win) // hop
    idx = np.arange(win)[None, :] + hop * np.arange(nwin)[:, None]
    sq = np.square(y, dtype=np.float64)[idx]
    win_rms = np.sqrt(np.mean(sq, axis=1))
    p10 = float(np.percentile(win_rms, 10.0))
    p50 = float(np.percentile(win_rms, 50.0))
    p95 = float(np.percentile(win_rms, 95.0))
    abs_floor = 1e-3  # -60 dBFS RMS: below this there is no usable signal
    # Noise floor estimate. p10 alone breaks when the signal fills the whole
    # file (e.g. a second-half capture with no pre-roll): then p10 is the tone
    # level and 4*p10 would hide the signal.  Clamping the noise estimate to
    # 10% of the median keeps the sustain detector working in that case.
    noise = min(p10, p50 * 0.1)
    thr = max(4.0 * noise, abs_floor)
    sustained = 3  # 3 windows * 5 ms hop = 30 ms above threshold
    onset_win = None
    loud_start = win_rms[0] > thr and (len(win_rms) < sustained
                                      or np.all(win_rms[:min(sustained, nwin)] > thr))
    if loud_start:
        onset_win = 0
    else:
        above = win_rms > thr
        for i in range(0, nwin - sustained + 1):
            if np.all(above[i:i + sustained]):
                onset_win = i
                break
    if onset_win is None:
        return {"detected": False, "onset_sample": None, "preroll_s": rnd(n / sr),
                "noise_rms": rnd(p10 if nwin else 0.0),
                "signal_rms": rnd(p95 if nwin else 0.0)}
    onset = int(onset_win * hop)
    # refine within one window: first sample of the sustained region that is
    # clearly above the local noise (half threshold), back off one hop.
    lo = max(0, onset - hop)
    hi = min(n, max(onset + win, lo + 1))
    seg = np.abs(y[lo:hi])
    cand = np.where(seg > 0.5 * thr)[0]
    onset = int(lo + cand[0]) if len(cand) else onset
    return {"detected": True, "onset_sample": onset,
            "preroll_s": rnd(onset / sr),
            "noise_rms": rnd(p10), "signal_rms": rnd(p95)}


# --------------------------------------------------------------------------- #
# tone helpers
# --------------------------------------------------------------------------- #
def estimate_tone_fft(y, sr):
    """Dominant frequency via Hann-windowed FFT + parabolic interpolation."""
    n = len(y)
    if n < 256:
        return None
    seg_len = int(min(n, max(4096, int(2.0 * sr))))
    if seg_len > n:
        seg_len = n
    start = max(0, (n - seg_len) // 2)
    seg = y[start:start + seg_len].astype(np.float64)
    seg = seg - np.mean(seg)
    w = np.hanning(seg_len)
    spec = np.abs(np.fft.rfft(seg * w))
    if len(spec) < 3:
        return None
    k = int(np.argmax(spec[1:]) + 1)
    if k <= 0 or k >= len(spec) - 1:
        freq = k * sr / float(seg_len)
        peak = float(spec[k])
    else:
        a, b, c = float(spec[k - 1]), float(spec[k]), float(spec[k + 1])
        denom = a - 2.0 * b + c
        delta = 0.5 * (a - c) / denom if denom != 0.0 else 0.0
        freq = (k + delta) * sr / float(seg_len)
        peak = b
    psd = np.square(spec)
    total = float(np.sum(psd))
    band = float(np.sum(psd[max(0, k - 3):min(len(psd), k + 4)]))
    frac = band / total if total > 0.0 else 0.0
    frac = min(max(frac, 1e-9), 1.0 - 1e-9)
    snr_db = 10.0 * math.log10(frac / (1.0 - frac))
    return {"freq_hz": rnd(freq, 4), "snr_db": rnd(snr_db, 2),
            "band_fraction": rnd(frac, 6),
            "bin_hz": rnd(sr / float(seg_len), 6)}


def estimate_tone_zc(y, sr):
    """Zero-crossing frequency estimate (DC removed).

    Uses the median interval between successive upward crossings, so a silent
    dropout gap does not bias the estimate (the long interval is an outlier).
    """
    if len(y) < int(0.2 * sr):
        return None
    yc = y - np.mean(y)
    if np.max(np.abs(yc)) < 1e-6:
        return None
    s = np.signbit(yc)
    up = np.where((~s[1:]) & s[:-1])[0]  # rising crossings (index of sample after)
    if len(up) < 8:
        return None
    intervals = np.diff(up).astype(np.float64)
    # keep intervals close to the median (discard dropout-gap outliers)
    med = float(np.median(intervals))
    if med <= 0.0:
        return None
    keep = np.abs(intervals - med) < 0.5 * med
    if np.sum(keep) < 4:
        return None
    return rnd(sr / float(np.median(intervals[keep])), 4)


def estimate_tone_recurrence(y, sr, expected_hz=None):
    """Exact tone frequency + purity via the second-order sine recurrence.

    For a pure sine at angular frequency w: y[n] = 2 cos(w) y[n-1] - y[n-2].
    Fit c = cos(w) by least squares over the largest usable contiguous region,
    then w = acos(c).  Returns frequency, amplitude, and recurrence residual.
    """
    n = len(y)
    if n < 64:
        return None
    # use a contiguous middle region, trimmed 5 ms at both ends
    trim = int(0.005 * sr)
    core = y[trim:n - trim] if n - 2 * trim > 256 else y
    if len(core) < 64:
        return None
    y0, y1, y2 = core[2:], core[1:-1], core[:-2]
    denom = 2.0 * float(np.dot(y1, y1))
    if denom <= 0.0:
        return None
    c = float(np.dot(y0 + y2, y1)) / denom
    c = max(-0.999999, min(0.999999, c))
    w = math.acos(c)
    freq = w * sr / TWO_PI
    amp = float(np.sqrt(2.0 * np.mean(np.square(core))))
    resid = y0 - 2.0 * c * y1 + y2
    return {"freq_hz": rnd(freq, 4), "w_rad_per_sample": w,
            "amp_est": rnd(amp, 6), "residual": resid,
            "offset": int(trim) + 2, "expected_hz": expected_hz}


def analytic_phase(y):
    """Hilbert analytic-signal phase/amplitude via FFT (no scipy)."""
    n = len(y)
    Y = np.fft.fft(y)
    h = np.zeros(n)
    if n % 2 == 0:
        h[0] = 1.0
        h[n // 2] = 1.0
        h[1:n // 2] = 2.0
    else:
        h[0] = 1.0
        h[1:(n + 1) // 2] = 2.0
    z = np.fft.ifft(Y * h)
    return np.angle(z), np.abs(z)


# --------------------------------------------------------------------------- #
# per-channel analysis
# --------------------------------------------------------------------------- #
def analyze_channel(y, sr, onset, opts):
    """Analyze one channel after onset. Returns a deterministic dict."""
    onset = int(onset)
    y2 = y[onset:]
    post_n = len(y2)
    has_signal = post_n > 0 and dbfs(rms(y2)) > -60.0
    post_rms = rms(y2)
    post_peak = float(np.max(np.abs(y2))) if post_n else 0.0

    out = {
        "channel": opts["channel"],
        "onset_sample": onset,
        "post_onset_frames": post_n,
        "post_onset_duration_s": rnd(post_n / sr),
        "rms_dbfs": rnd(dbfs(post_rms)),
        "peak_dbfs": rnd(dbfs(post_peak)),
        "clipped": bool(post_peak >= 0.999),
        "gaps": {},
        "frozen_runs": {},
        "deltas": {},
        "tone": None,
        "verdict": {"result": "PASS", "reasons": [], "warnings": []},
    }
    reasons = out["verdict"]["reasons"]
    warnings = out["verdict"]["warnings"]

    if post_n == 0:
        reasons.append("no_audio_after_onset")
        out["verdict"]["result"] = "FAIL"
        return out

    # ---- silent-dropout (zero) runs --------------------------------------
    min_gap = max(2, int(round(opts["min_gap_ms"] * 1e-3 * sr)))
    zero_thr = opts["zero_threshold"]
    zero_mask = np.abs(y2) < zero_thr
    gap_runs = [(a, b) for a, b in runs_of_mask(zero_mask) if (b - a) >= min_gap]
    grace = int(round(opts["startup_window_ms"] * 1e-3 * sr))
    gap_details = []
    steady = []
    startup = []
    for a, b in gap_runs:
        dur_ms = (b - a) / sr * 1000.0
        item = {"start_s": rnd(a / sr), "start_rel_s": rnd(a / sr),
                "dur_ms": rnd(dur_ms, 3),
                "level": rnd(float(np.max(np.abs(y2[a:b]))) if b > a else 0.0)}
        gap_details.append(item)
        (startup if a < grace else steady).append(item)
    out["gaps"] = {
        "threshold": zero_thr,
        "min_ms": opts["min_gap_ms"],
        "count": len(gap_details),
        "total_ms": rnd(sum(g["dur_ms"] for g in gap_details), 3),
        "max_ms": rnd(max([g["dur_ms"] for g in gap_details], default=0.0), 3),
        "steady_count": len(steady),
        "steady_total_ms": rnd(sum(g["dur_ms"] for g in steady), 3),
        "steady_max_ms": rnd(max([g["dur_ms"] for g in steady], default=0.0), 3),
        "startup_count": len(startup),
        "startup_total_ms": rnd(sum(g["dur_ms"] for g in startup), 3),
        "details": gap_details[:50],
        "details_truncated": bool(len(gap_details) > 50),
    }

    # ---- frozen / held runs (identical consecutive samples) --------------
    frozen_mask = np.zeros(post_n, dtype=bool)
    if post_n > 1:
        diff_abs = np.abs(np.diff(y2))
        frozen_mask[1:] = diff_abs == 0.0
        # frozen_mask[j] True <=> y2[j] == y2[j-1]. A maximal True run
        # [a, b) therefore covers equal samples y2[a-1 .. b-1].
        frozen_runs = [(a - 1, b) for a, b in runs_of_mask(frozen_mask)
                       if (b - (a - 1)) >= min_gap]
    else:
        frozen_runs = []
    frozen_details = []
    held_runs = []
    for a, b in frozen_runs:
        lvl = float(np.max(np.abs(y2[a:b])))
        is_held = lvl >= zero_thr
        item = {"start_s": rnd(a / sr), "dur_ms": rnd((b - a) / sr * 1000.0, 3),
                "level": rnd(lvl), "held": bool(is_held),
                "class": "startup_transient" if (a / sr * 1000.0) < opts["startup_window_ms"]
                         else "steady"}
        frozen_details.append(item)
        if is_held:
            held_runs.append(item)
    startup_held = [f for f in held_runs if f["class"] == "startup_transient"]
    steady_held = [f for f in held_runs if f["class"] != "startup_transient"]
    out["frozen_runs"] = {
        "count": len(frozen_details),
        "total_ms": rnd(sum(f["dur_ms"] for f in frozen_details), 3),
        "max_ms": rnd(max([f["dur_ms"] for f in frozen_details], default=0.0), 3),
        "held_count": len(held_runs),
        "held_total_ms": rnd(sum(f["dur_ms"] for f in held_runs), 3),
        "startup_held_count": len(startup_held),
        "startup_held_total_ms": rnd(sum(f["dur_ms"] for f in startup_held), 3),
        "steady_held_count": len(steady_held),
        "steady_held_total_ms": rnd(sum(f["dur_ms"] for f in steady_held), 3),
        "held_details": held_runs[:20],
    }

    # ---- adjacent-sample deltas / clicks --------------------------------
    if post_n > 2:
        d = np.abs(np.diff(y2))
        med = float(np.median(d))
        mad = float(np.median(np.abs(d - med)))
        robust_sigma = 1.4826 * mad
        delta_thr = med + 20.0 * max(robust_sigma, 1e-7)
        outlier_mask = d > delta_thr
        # clicks/splices next to a known silent gap are expected gap boundaries
        guard = int(round(0.001 * sr))
        delta_bad = np.zeros(len(d), dtype=bool)
        for ga, gb in gap_runs:
            delta_bad[max(0, ga - guard):min(len(d), gb + guard)] = True
        outlier_runs = runs_of_mask(outlier_mask)
        unexplained = [(a, b) for a, b in outlier_runs
                       if not np.any(delta_bad[a:b + 1])]
        out["deltas"] = {
            "max": rnd(float(np.max(d))),
            "p999": rnd(float(np.percentile(d, 99.9))),
            "median": rnd(med),
            "robust_sigma": rnd(robust_sigma),
            "outlier_threshold": rnd(delta_thr),
            "outlier_count": len(outlier_runs),
            "unexplained_outlier_count": len(unexplained),
            "outlier_details": [
                {"at_s": rnd((a + 1) / sr), "dur_ms": rnd((b - a) / sr * 1000.0, 3),
                 "delta": rnd(float(np.max(d[a:b])))}
                for a, b in outlier_runs[:20]],
            "unexplained_details": [
                {"at_s": rnd((a + 1) / sr),
                 "delta": rnd(float(np.max(d[a:b])))}
                for a, b in unexplained[:20]],
        }
    else:
        out["deltas"] = {"max": 0.0, "p999": 0.0, "median": 0.0,
                         "outlier_count": 0, "outlier_details": []}

    # ---- tone analysis ---------------------------------------------------
    expected_hz = opts["tone_hz"]
    if expected_hz is not None or opts["tone_detect"]:
        tone = {}
        fft_est = estimate_tone_fft(y2, sr)
        zc_est = estimate_tone_zc(y2, sr)
        rec = estimate_tone_recurrence(y2, sr, expected_hz)
        if rec is not None:
            f_recur = rec["freq_hz"]
            w = rec["w_rad_per_sample"]
        elif fft_est is not None:
            f_recur = fft_est["freq_hz"]
            w = TWO_PI * f_recur / sr
        else:
            f_recur = None
            w = None
        detected = bool(post_rms > 1e-3 and (fft_est is not None)
                        and fft_est["band_fraction"] >= 0.5)
        tone["detected"] = detected
        tone["expected_hz"] = expected_hz
        tone["freq_fft_hz"] = fft_est["freq_hz"] if fft_est else None
        tone["fft_snr_db"] = fft_est["snr_db"] if fft_est else None
        tone["fft_band_fraction"] = fft_est["band_fraction"] if fft_est else None
        tone["freq_zc_hz"] = zc_est
        tone["freq_recur_hz"] = rnd(f_recur, 4) if f_recur is not None else None
        tone["amp_recur"] = rec["amp_est"] if rec else None
        if expected_hz is not None and f_recur is not None:
            err = f_recur - expected_hz
            tone["freq_error_hz"] = rnd(err, 4)
            tol = max(0.005 * abs(expected_hz), 2.0)
            tone["freq_tolerance_hz"] = rnd(tol, 4)
            tone["freq_ok"] = bool(abs(err) <= tol)
        else:
            tone["freq_error_hz"] = None
            tone["freq_ok"] = None

        # phase continuity, only when we have a usable tone estimate
        phase_info = None
        if detected and f_recur is not None and w is not None and w > 1e-6:
            phase_info = analyze_phase(y2, sr, w, opts, gap_runs)
            tone["phase"] = phase_info
        else:
            tone["phase"] = None

        # recurrence splice detection
        rec_info = None
        if detected and rec is not None and w is not None:
            rec_info = analyze_recurrence(y2, rec["residual"], rec["amp_est"],
                                          sr, w, opts, gap_runs,
                                          offset=rec.get("offset", 2))
            tone["recurrence"] = rec_info
        else:
            tone["recurrence"] = None

        # verdict contributions
        if expected_hz is not None:
            if not detected:
                reasons.append("tone_not_detected")
            elif tone.get("freq_ok") is False:
                reasons.append("tone_frequency_error")
        if opts["tone_level"] is not None:
            exp_rms = float(opts["tone_level"]) / math.sqrt(2.0)
            db_err = dbfs(post_rms) - dbfs(exp_rms)
            tone["expected_level_dbfs"] = rnd(dbfs(exp_rms))
            tone["level_error_db"] = rnd(db_err)
            if abs(db_err) > 3.0:
                reasons.append("tone_level_error")
        if phase_info is not None:
            steady_jumps = phase_info.get("steady_jump_events", phase_info["jump_events"])
            startup_jumps = phase_info.get("startup_jump_events", 0)
            if steady_jumps > 0:
                reasons.append("phase_discontinuity")
            if startup_jumps > 0:
                warnings.append("startup_phase_jump_at_onset=%d" % startup_jumps)
            if phase_info["unresolved_events"] > 0:
                warnings.append("phase_jump_wrap_ambiguous")
        if rec_info is not None:
            steady_real = rec_info.get("steady_events_real", rec_info.get("events_real", 0))
            startup_real = rec_info.get("startup_events_real", 0)
            if steady_real > 0:
                reasons.append("sample_splice")
            if startup_real > 0:
                warnings.append("startup_splice_at_onset=%d" % startup_real)
            if rec_info.get("events_subsample", 0) > 0:
                warnings.append("subsample_resampler_artifacts")
        out["tone"] = tone

    # ---- generic verdict criteria ---------------------------------------
    if opts["expect_signal"] and not has_signal:
        reasons.append("no_signal_after_onset")
    if out["gaps"]["steady_count"] > 0:
        reasons.append("silent_dropouts")
    frozen_steady = out["frozen_runs"].get("steady_held_count", out["frozen_runs"]["held_count"])
    frozen_startup = out["frozen_runs"].get("startup_held_count", 0)
    if frozen_steady > 0:
        reasons.append("held_samples")
    if frozen_startup > 0:
        warnings.append("startup_held_run_at_onset=%d" % frozen_startup)
    if out["clipped"]:
        reasons.append("clipping")
    if out["gaps"]["startup_count"] > 0:
        warnings.append("startup_gap_at_onset=%d" % out["gaps"]["startup_count"])

    exp_frames = opts["expected_frames"]
    if exp_frames:
        shortfall = int(exp_frames) - post_n
        out["capture"] = {
            "expected_frames": int(exp_frames),
            "actual_post_onset_frames": post_n,
            "shortfall_frames": shortfall,
            "shortfall_ms": rnd(shortfall / sr * 1000.0, 3),
        }
        if shortfall > opts["max_frame_shortfall"]:
            reasons.append("capture_shortfall")
    else:
        out["capture"] = None

    if reasons:
        out["verdict"]["result"] = "FAIL"
    return out


def analyze_phase(y, sr, w, opts, gap_runs):
    """Tone phase continuity: drift (ppm) and phase-jump/skip events.

    Uses the analytic-signal phase. For consecutive valid sample pairs i<j:
        jump = wrap(phase[j] - phase[i] - w*(j-i))
    A nonzero jump means the tone's phase does not advance with elapsed time,
    i.e. samples were skipped (jump>0) or duplicated (jump<0) between i and j.
    j-i is normally 1; across a dropout gap it is the gap length, and an
    inserted-silence gap is phase-continuous (jump ~ 0). Reported frame counts
    are resolvable up to +/- half a cycle (-/+ sr/(2f) frames); bigger jumps
    are additionally flagged as wrap-ambiguous.
    """
    phase, env = analytic_phase(y)
    env_max = float(np.max(env)) if len(env) else 0.0
    if env_max <= 0.0:
        return {"valid_fraction": 0.0, "drift_ppm": None, "jump_events": 0,
                "unresolved_events": 0, "missing_frames": 0, "extra_frames": 0,
                "details": []}
    valid = env > max(0.05 * env_max, 1e-3)
    n = len(y)
    trim = int(0.005 * sr)
    valid[:trim] = False
    valid[n - trim:] = False
    # exclude 2 ms around silent-gap boundaries (splices are expected there)
    guard = int(round(0.002 * sr))
    for a, b in gap_runs:
        valid[max(0, a - guard):min(n, a + guard)] = False
        valid[max(0, b - guard):min(n, b + guard)] = False
    vi = np.where(valid)[0]
    if len(vi) < 32:
        return {"valid_fraction": rnd(float(np.mean(valid))), "drift_ppm": None,
                "jump_events": 0, "unresolved_events": 0, "missing_frames": 0,
                "extra_frames": 0, "details": []}
    # Clock/tone drift: median phase advance between adjacent valid samples.
    # (A linear fit over the whole file is biased by the random analytic phase
    # inside dropout gaps, so use the robust local estimator.)
    ppm = None
    if len(vi) > 4:
        adj = vi[1:][np.diff(vi) == 1]
        prev = vi[:-1][np.diff(vi) == 1]
        if len(adj) > 4:
            dphi = wrap_angle(phase[adj] - phase[prev])
            w_obs = float(np.median(dphi))
            if w > 0.0:
                ppm = (w_obs - w) / w * 1e6
    # residual phase after removing the nominal advance, per sample
    ev = vi[1:]
    pv = vi[:-1]
    elapsed = (ev - pv).astype(np.float64)
    jump = wrap_angle(phase[ev] - phase[pv] - w * elapsed)
    thr = 0.25 if opts is None else float(opts.get("phase_jump_thr", 0.25))
    bad = np.abs(jump) > thr
    idx = np.where(bad)[0]
    jump_events = []
    missing = 0
    extra = 0
    unresolved = 0
    for j in idx[:200]:
        frames = float(jump[j]) / w
        item = {"at_s": rnd(int(ev[j]) / sr), "jump_rad": rnd(float(jump[j]), 4),
                "implied_frames": rnd(frames, 2),
                "elapsed_samples": int(elapsed[j])}
        if abs(float(jump[j])) <= math.pi / 2.0:
            nf = int(round(frames))
            item["frames"] = nf
            if nf > 0:
                missing += nf
            else:
                extra += -nf
        else:
            item["frames"] = None
            unresolved += 1
        jump_events.append(item)
    # count over all events (details capped at 50)
    if len(idx) > 50:
        for j in idx[50:]:
            jj = float(jump[j])
            frames = jj / w
            if abs(jj) <= math.pi / 2.0:
                nf = int(round(frames))
                if nf > 0:
                    missing += nf
                else:
                    extra += -nf
            else:
                unresolved += 1
    startup_window_s = 0.0 if opts is None else float(opts.get("startup_window_ms", 50.0)) * 1e-3
    startup_jump = 0
    steady_jump = 0
    for item in jump_events:
        item["class"] = ("startup_transient" if item["at_s"] < startup_window_s else "steady")
    for j in idx:
        if int(ev[j]) / sr < startup_window_s:
            startup_jump += 1
        else:
            steady_jump += 1
    # count classification over all events (details capped at 50)
    startup_jump = sum(1 for j in idx if int(ev[j]) / sr < startup_window_s)
    steady_jump = int(len(idx)) - startup_jump
    return {
        "valid_fraction": rnd(float(np.mean(valid))),
        "samples_used": int(len(vi)),
        "drift_ppm": rnd(ppm, 3),
        "jump_events": int(len(idx)),
        "startup_jump_events": int(startup_jump),
        "steady_jump_events": int(steady_jump),
        "unresolved_events": unresolved,
        "missing_frames": missing,
        "extra_frames": extra,
        "details": jump_events[:50],
        "details_truncated": bool(len(idx) > 50),
    }


def analyze_recurrence(y, residual, amp, sr, w, opts, gap_runs, offset=2):
    """Detect high-frequency splices with the pure-tone recurrence residual.

    Detection: |y[n] - (2cos(w)y[n-1] - y[n-2])| beyond max(8*sigma_resid, 2e-4)
    away from known silent gaps and signal ends.

    Classification (frozen semantics, thresholds documented in
    reports/engine/ENGINE_AB_REPORT.md):
      real splice      : local recurrence deviation > max(8*sigma_resid, 4e-3)
                         AND adjacent-sample delta outlier vs the expected tone
                         derivative (delta > 1.5 * amp * w);
      sub-sample       : any other detected residual event (fractional
                         resampler-trim artifact: tiny deviation, delta still
                         within the tone derivative envelope).
    PASS requires zero real splices; sub-sample artifacts are only warnings.
    """
    n = len(y)
    empty = {"events": 0, "events_real": 0, "events_subsample": 0,
             "startup_events_real": 0, "steady_events_real": 0,
             "startup_events_subsample": 0, "steady_events_subsample": 0,
             "details": []}
    if residual is None or len(residual) < 32:
        return empty
    trim = int(0.005 * sr)
    guard = int(round(max(0.002, 0.5 * opts.get("min_gap_ms", 2.0) * 1e-3) * sr))
    bad = np.zeros(n, dtype=bool)
    bad[:trim] = True
    bad[n - trim:] = True
    # map gap boundaries to the recurrence-residual sample coordinates
    for a, b in gap_runs:
        bad[max(0, a - guard):min(n, b + guard)] = True
    rr = residual
    rr_idx = np.arange(len(rr)) + int(offset)
    rr_bad = bad[rr_idx] if len(rr_idx) else np.zeros(0, dtype=bool)
    rr_valid = rr[~rr_bad]
    if len(rr_valid) == 0:
        return empty
    med = float(np.median(rr_valid))
    sigma_resid = 1.4826 * float(np.median(np.abs(rr_valid - med)))
    thr = max(8.0 * sigma_resid, 2e-4)
    mask = np.abs(rr - med) > thr
    mask &= ~rr_bad
    runs = runs_of_mask(mask)

    # Splice classification (frozen semantics).
    dy = np.abs(np.diff(y)) if len(y) > 1 else np.zeros(0)
    expected_deriv = float(max(amp, 1e-6) * w)  # max |dy/dn| of the fitted tone
    delta_outlier_thr = 1.5 * expected_deriv
    real_deviation_thr = max(8.0 * sigma_resid, 4e-3)
    details = []
    real_count = 0
    subsample_count = 0
    startup_real_count = 0
    steady_real_count = 0
    startup_sub_count = 0
    steady_sub_count = 0
    for a, b in runs:
        idx_start = int(rr_idx[a])
        idx_end = int(rr_idx[b - 1]) if b > a else idx_start
        dev = float(np.max(np.abs(rr[a:b] - med)))
        d_lo = max(0, idx_start - 1)
        d_hi = min(len(dy), idx_end + 1)
        dmax = float(np.max(dy[d_lo:d_hi])) if d_hi > d_lo else 0.0
        delta_outlier = bool(dmax > delta_outlier_thr)
        is_real = bool(dev > real_deviation_thr and delta_outlier)
        at_s = rnd(idx_start / sr)
        is_startup = at_s < (0.0 if opts is None else float(opts.get("startup_window_ms", 50.0)) * 1e-3)
        if is_real:
            real_count += 1
            if is_startup:
                startup_real_count += 1
            else:
                steady_real_count += 1
        else:
            subsample_count += 1
            if is_startup:
                startup_sub_count += 1
            else:
                steady_sub_count += 1
        details.append({"at_s": at_s,
                        "dur_ms": rnd((b - a) / sr * 1000.0, 3),
                        "residual": rnd(dev),
                        "delta": rnd(dmax),
                        "delta_outlier": delta_outlier,
                        "startup": bool(is_startup),
                        "class": ("startup_transient" if is_startup else "steady") + ":" +
                                 ("real_splice" if is_real else "subsample_artifact")})
    return {"events": len(runs), "events_real": real_count,
            "events_subsample": subsample_count,
            "startup_events_real": startup_real_count,
            "steady_events_real": steady_real_count,
            "startup_events_subsample": startup_sub_count,
            "steady_events_subsample": steady_sub_count,
            "threshold": rnd(thr), "residual_sigma": rnd(sigma_resid),
            "real_deviation_threshold": rnd(real_deviation_thr),
            "expected_deriv": rnd(expected_deriv),
            "delta_outlier_threshold": rnd(delta_outlier_thr),
            "details": details[:50], "details_truncated": bool(len(runs) > 50)}


# --------------------------------------------------------------------------- #
# file-level driver
# --------------------------------------------------------------------------- #
def analyze_file(path, opts):
    x, sr, bits, fmt = load_wav(path)
    nframes, nch = x.shape
    result = {
        "analyzer": {"name": ANALYZER_NAME, "version": ANALYZER_VERSION},
        "label": opts.get("label"),
        "file": os.path.abspath(path),
        "sha256": sha256_file(path),
        "bytes": os.path.getsize(path),
        "audio": {"format": fmt, "sample_rate": sr, "channels": nch,
                  "bits": bits, "frames": nframes,
                  "duration_s": rnd(nframes / sr)},
        "options": {
            "tone_hz": opts["tone_hz"],
            "tone_level": opts["tone_level"],
            "skip_preroll": opts["skip_preroll"],
            "zero_threshold": opts["zero_threshold"],
            "min_gap_ms": opts["min_gap_ms"],
            "startup_window_ms": opts["startup_window_ms"],
            "startup_grace_ms": opts["startup_window_ms"],
            "record_window_s": opts["record_window_s"],
            "expect_signal": opts["expect_signal"],
            "expect_silence": opts["expect_silence"],
        },
        "channels": [],
        "verdict": {"result": "ERROR", "reasons": [], "warnings": []},
    }
    reasons = result["verdict"]["reasons"]
    warnings = result["verdict"]["warnings"]

    # channels to analyze
    chans = list(range(nch)) if opts["channel"] is None else [opts["channel"]]
    chans = [c for c in chans if 0 <= c < nch]
    if not chans:
        raise ValueError("channel %r out of range (channels=%d)" % (opts["channel"], nch))

    # onset is detected per channel; for a multi-channel file take the earliest
    onset_per_ch = {}
    for c in chans:
        y = x[:, c]
        od = detect_onset(y, sr) if opts["skip_preroll"] else {
            "detected": True, "onset_sample": 0, "preroll_s": 0.0,
            "noise_rms": None, "signal_rms": None}
        onset_per_ch[c] = od

    # per-channel analysis
    for c in chans:
        od = onset_per_ch[c]
        opts_c = dict(opts)
        opts_c["channel"] = c
        if opts["tone_pattern"] is not None:
            opts_c["tone_hz"] = opts["tone_pattern"][0] + opts["tone_pattern"][1] * c
        if not od["detected"]:
            # silence / no signal
            y = x[:, c]
            ch = {"channel": c, "onset_sample": None,
                  "post_onset_frames": 0, "post_onset_duration_s": 0.0,
                  "rms_dbfs": rnd(dbfs(rms(y))), "peak_dbfs": rnd(dbfs(float(np.max(np.abs(y)))) if len(y) else 0.0),
                  "clipped": False, "gaps": None, "frozen_runs": None,
                  "deltas": None, "tone": None, "capture": None,
                  "verdict": {"result": "PASS", "reasons": [], "warnings": []}}
            if opts["expect_silence"]:
                if rms(y) > 1e-4:
                    ch["verdict"]["result"] = "FAIL"
                    ch["verdict"]["reasons"].append("unexpected_signal")
                else:
                    ch["verdict"]["warnings"].append("no_signal_expected_silence")
                if opts["record_window_s"]:
                    exp = int(round(opts["record_window_s"] * sr))
                    ch["capture"] = {"expected_frames": exp,
                                     "actual_frames": nframes,
                                     "shortfall_frames": exp - nframes,
                                     "shortfall_ms": rnd((exp - nframes) / sr * 1000.0, 3)}
            else:
                ch["verdict"]["result"] = "FAIL"
                ch["verdict"]["reasons"].append("no_signal")
            result["channels"].append(ch)
            continue
        onset = int(od["onset_sample"])
        exp_frames = None
        if opts["record_window_s"]:
            exp_frames = max(0, int(round(opts["record_window_s"] * sr)) - onset)
        opts_c["expected_frames"] = exp_frames
        ch = analyze_channel(x[:, c], sr, onset, opts_c)
        ch["preroll_s"] = rnd(onset / sr)
        ch["onset_detected"] = True
        # onset diagnostics
        ch["onset"] = {"preroll_s": rnd(onset / sr),
                       "noise_rms_dbfs": rnd(dbfs(od["noise_rms"] or 0.0)),
                       "window_rms_dbfs": rnd(dbfs(od["signal_rms"] or 0.0))}
        result["channels"].append(ch)

    # aggregate verdict
    all_reasons = []
    all_warnings = []
    for ch in result["channels"]:
        if ch["verdict"]["result"] == "FAIL":
            all_reasons.extend("%s: %s" % (ch["channel"], r) for r in ch["verdict"]["reasons"])
        all_warnings.extend("%s: %s" % (ch["channel"], w) for w in ch["verdict"]["warnings"])
    result["verdict"]["reasons"] = all_reasons
    result["verdict"]["warnings"] = all_warnings
    result["verdict"]["result"] = "FAIL" if all_reasons else "PASS"
    # global capture check (recorder reported frames vs record window)
    if opts["record_window_s"]:
        exp = int(round(opts["record_window_s"] * sr))
        short = exp - nframes
        result["capture"] = {
            "record_window_s": rnd(opts["record_window_s"]),
            "expected_frames": exp,
            "actual_frames": nframes,
            "shortfall_frames": short,
            "shortfall_ms": rnd(short / sr * 1000.0, 3),
            "frames_ok": bool(short <= 0.25 * sr),
        }
        if short > 0.25 * sr:
            reasons.append("global_capture_shortfall")
            result["verdict"]["result"] = "FAIL"
    else:
        result["capture"] = None
    return result


# --------------------------------------------------------------------------- #
# human summary
# --------------------------------------------------------------------------- #
def human_summary(res):
    lines = []
    a = res["audio"]
    lines.append("WAV %s" % res["file"])
    lines.append("  audio: sr=%d ch=%d bits=%d frames=%d dur=%.3fs sha256=%s"
                 % (a["sample_rate"], a["channels"], a["bits"], a["frames"],
                    a["duration_s"], res["sha256"][:16]))
    if "capture" in res and res.get("capture"):
        c = res["capture"]
        lines.append("  capture: %d frames (expected %d, shortfall %d = %.1f ms, ok=%s)"
                     % (a["frames"], c["expected_frames"], c["shortfall_frames"],
                        c["shortfall_ms"], c.get("frames_ok")))
    for ch in res["channels"]:
        cid = ch["channel"]
        if ch.get("onset_detected") is False or ch.get("onset_sample") is None:
            lines.append("  ch%d: no signal onset; rms=%.2f dBFS peak=%.2f dBFS"
                         % (cid, ch["rms_dbfs"], ch["peak_dbfs"]))
            continue
        g = ch["gaps"]
        fr = ch["frozen_runs"]
        lines.append("  ch%d: preroll=%.3fs post=%.3fs rms=%.2f dBFS peak=%.2f dBFS"
                     % (cid, ch["preroll_s"], ch["post_onset_duration_s"],
                        ch["rms_dbfs"], ch["peak_dbfs"]))
        lines.append("    dropouts: steady=%d total=%.1fms max=%.1fms | startup=%d %.1fms"
                     % (g["steady_count"], g["steady_total_ms"], g["steady_max_ms"],
                        g["startup_count"], g["startup_total_ms"]))
        if g["steady_count"]:
            first = ", ".join("t=%.3fs/%.1fms" % (d["start_s"], d["dur_ms"])
                              for d in g["details"][:6])
            lines.append("      first gaps: " + first)
        lines.append("    frozen runs: n=%d total=%.1fms max=%.1fms held(nonzero)=%d steady=%d startup=%d"
                     % (fr["count"], fr["total_ms"], fr["max_ms"], fr["held_count"],
                        fr.get("steady_held_count", fr["held_count"]),
                        fr.get("startup_held_count", 0)))
        d = ch["deltas"]
        lines.append("    deltas: max=%.5f p99.9=%.5f median=%.6f outlier_events=%d unexplained=%d"
                     % (d["max"], d["p999"], d["median"], d["outlier_count"],
                        d.get("unexplained_outlier_count", 0)))
        t = ch.get("tone")
        if t:
            lines.append("    tone: expected=%s detected=%s fft=%s zc=%s recur=%s"
                         % (t["expected_hz"], t["detected"], t["freq_fft_hz"],
                            t["freq_zc_hz"], t["freq_recur_hz"]))
            if t.get("freq_error_hz") is not None:
                lines.append("      freq_err=%+.4f Hz (tol %.2f) ok=%s"
                             % (t["freq_error_hz"], t["freq_tolerance_hz"], t["freq_ok"]))
            p = t.get("phase")
            if p:
                drift = p["drift_ppm"]
                lines.append("      phase: valid=%.3f drift=%s ppm jump_events=%d (steady=%d startup=%d) missing=%d extra=%d unresolved=%d"
                             % (p["valid_fraction"],
                                ("%+.2f" % drift) if drift is not None else "n/a",
                                p["jump_events"],
                                p.get("steady_jump_events", p["jump_events"]),
                                p.get("startup_jump_events", 0),
                                p["missing_frames"], p["extra_frames"],
                                p["unresolved_events"]))
            r = t.get("recurrence")
            if r:
                lines.append("      recurrence: splices_real=%d (steady=%d startup=%d) splices_subsample=%d sigma=%.2g"
                             % (r.get("events_real", 0),
                                r.get("steady_events_real", r.get("events_real", 0)),
                                r.get("startup_events_real", 0),
                                r.get("events_subsample", 0),
                                r.get("residual_sigma") or 0.0))
    v = res["verdict"]
    lines.append("VERDICT: %s" % v["result"])
    for r in v["reasons"]:
        lines.append("  reason: %s" % r)
    for w in v["warnings"]:
        lines.append("  warning: %s" % w)
    return "\n".join(lines)


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
def parse_tone(spec):
    """'1000' -> (1000, None); '1000+40' -> (1000, 40) pattern per channel."""
    if spec is None:
        return None, None
    if "+" in spec:
        base, step = spec.split("+", 1)
        return float(base), float(step)
    return float(spec), None


def main(argv=None):
    ap = argparse.ArgumentParser(description="NL Audio Matrix continuity analyzer")
    ap.add_argument("wav", nargs="+", help="WAV file(s) to analyze")
    ap.add_argument("--tone", default=None,
                    help="expected tone frequency in Hz, or pattern '1000+40' (per channel)")
    ap.add_argument("--tone-level", type=float, default=None,
                    help="nominal tone amplitude 0..1 (e.g. 0.25) for level check")
    ap.add_argument("--tone-detect", action="store_true",
                    help="detect tone even when --tone is not given")
    ap.add_argument("--record-window", type=float, default=None,
                    help="expected capture window in seconds (for frame accounting)")
    ap.add_argument("--skip-preroll", dest="skip_preroll", action="store_true",
                    default=True, help="ignore audio before signal onset (default)")
    ap.add_argument("--no-skip-preroll", dest="skip_preroll", action="store_false",
                    help="analyze from sample 0")
    ap.add_argument("--zero-threshold", type=float, default=1e-4,
                    help="|sample| below this is 'silence' (default 1e-4)")
    ap.add_argument("--min-gap-ms", type=float, default=2.0,
                    help="minimum silent-run length counted as a dropout (default 2 ms)")
    ap.add_argument("--startup-window-ms", type=float, default=50.0,
                    help="events starting within this window after onset are startup "
                         "transients (default 50 ms)")
    ap.add_argument("--startup-grace-ms", type=float, default=None,
                    help="DEPRECATED alias; overrides --startup-window-ms when given")
    ap.add_argument("--phase-jump-thr", type=float, default=0.25,
                    help="phase residual threshold in radians for a discontinuity")
    ap.add_argument("--max-frame-shortfall", type=int, default=None,
                    help="allowed capture shortfall in frames (default 0.25 s)")
    ap.add_argument("--expect-signal", action="store_true",
                    help="FAIL if no signal onset is found")
    ap.add_argument("--expect-silence", action="store_true",
                    help="PASS only if the capture is silent (control run)")
    ap.add_argument("--channel", type=int, default=None, help="analyze this channel only")
    ap.add_argument("--label", default=None)
    ap.add_argument("--json", default=None, help="write deterministic JSON here")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    tone_hz, tone_step = parse_tone(args.tone)
    max_shortfall = (args.max_frame_shortfall if args.max_frame_shortfall is not None
                     else int(0.25 * 48000))
    opts = {
        "tone_hz": tone_hz,
        "tone_pattern": (tone_hz, tone_step) if tone_step is not None else None,
        "tone_level": args.tone_level,
        "tone_detect": args.tone_detect,
        "record_window_s": args.record_window,
        "skip_preroll": args.skip_preroll,
        "zero_threshold": args.zero_threshold,
        "min_gap_ms": args.min_gap_ms,
        "startup_window_ms": (args.startup_grace_ms if args.startup_grace_ms is not None
                              else args.startup_window_ms),
        "phase_jump_thr": args.phase_jump_thr,
        "expect_signal": args.expect_signal,
        "expect_silence": args.expect_silence,
        "channel": args.channel,
        "label": args.label,
        "max_frame_shortfall": max_shortfall,
    }
    results = []
    worst = 0
    for path in args.wav:
        try:
            res = analyze_file(path, opts)
        except Exception as exc:  # noqa: BLE001 - report as analyzer error
            print("ANALYZER ERROR: %s: %s" % (path, exc), file=sys.stderr)
            return 2
        results.append(res)
        if not args.quiet:
            print(human_summary(res))
            print("")
        if res["verdict"]["result"] != "PASS":
            worst = 1
    payload = results[0] if len(results) == 1 else {"files": results}
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=1, sort_keys=True, ensure_ascii=True)
            f.write("\n")
    if len(results) > 1:
        print("SUMMARY: %d file(s), %d PASS, %d FAIL"
              % (len(results), sum(1 for r in results if r["verdict"]["result"] == "PASS"),
                 sum(1 for r in results if r["verdict"]["result"] != "PASS")))
    return worst


if __name__ == "__main__":
    sys.exit(main())
