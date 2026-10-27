#!/usr/bin/env python3
"""analyze_smoke.py - continuity metrics for the ABI v3 capture smoke WAV.

Usage: python analyze_smoke.py <wav> [expected_tone_hz]
Prints FILE / RMS / peak / zero-run and flat-run statistics / dominant tone.
Read-only; no external services."""
import sys, os, wave
import numpy as np

def load(path):
    w = wave.open(path, 'rb')
    n = w.getnframes(); ch = w.getnchannels(); sw = w.getsampwidth(); sr = w.getframerate()
    raw = w.readframes(n); w.close()
    if sw == 2:
        x = np.frombuffer(raw, dtype='<i2').astype(np.float64) / 32768.0
    elif sw == 4:
        x = np.frombuffer(raw, dtype='<f4').astype(np.float64)
    elif sw == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3)
        v = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16))
        v = np.where(v >= (1 << 23), v - (1 << 24), v)
        x = v.astype(np.float64) / (1 << 23)
    else:
        x = np.frombuffer(raw, dtype='<i4').astype(np.float64) / (1 << 31)
    return x.reshape(-1, ch), sr, sw

def runs(mask):
    d = np.diff(mask.astype(np.int8))
    starts = np.where(d == 1)[0] + 1
    ends = np.where(d == -1)[0] + 1
    if len(mask) and mask[0]: starts = np.r_[0, starts]
    if len(mask) and mask[-1]: ends = np.r_[ends, len(mask)]
    return list(zip(starts.tolist(), ends.tolist()))

def dominant_hz(y, sr):
    if len(y) < 256: return 0.0
    y = y - np.mean(y)
    spec = np.abs(np.fft.rfft(y * np.hanning(len(y))))
    if len(spec) < 2: return 0.0
    return float(np.argmax(spec) * sr / len(y))

def analyze(path, expected_hz):
    x, sr, sw = load(path)
    print("FILE %s sr=%d ch=%d bits=%d frames=%d dur=%.3fs" % (
        os.path.basename(path), sr, x.shape[1], sw * 8, x.shape[0], x.shape[0] / sr))
    for c in range(x.shape[1]):
        y = x[:, c]
        rms = float(np.sqrt(np.mean(y ** 2))) if len(y) else 0.0
        peak = float(np.max(np.abs(y))) if len(y) else 0.0
        print("  ch%d rms=%.2f dBFS peak=%.2f dBFS dc=%.6f" % (
            c, 20 * np.log10(max(rms, 1e-12)), 20 * np.log10(max(peak, 1e-12)), float(np.mean(y))))
        zr = [(a, b) for a, b in runs(np.abs(y) < 1e-4) if (b - a) >= int(0.002 * sr)]
        print("    zero-runs>=2ms n=%d total=%.1fms max=%.1fms first=%s" % (
            len(zr), sum(b - a for a, b in zr) * 1000.0 / sr,
            (max(b - a for a, b in zr) * 1000.0 / sr) if zr else 0.0,
            [(round(a / sr, 3), round((b - a) * 1000.0 / sr, 1)) for a, b in zr[:6]]))
        d = np.abs(np.diff(y))
        fr = [(a, b + 1) for a, b in runs(d < 1e-7) if (b + 1 - a) >= int(0.002 * sr)]
        print("    flat-runs>=2ms n=%d total=%.1fms max=%.1fms first=%s" % (
            len(fr), sum(b - a for a, b in fr) * 1000.0 / sr,
            (max(b - a for a, b in fr) * 1000.0 / sr) if fr else 0.0,
            [(round(a / sr, 3), round((b - a) * 1000.0 / sr, 1)) for a, b in fr[:6]]))
        hz = dominant_hz(y, sr)
        print("    dominant=%.1f Hz expected=%s delta=%s" % (
            hz, expected_hz, ("%.1f" % (hz - expected_hz)) if expected_hz else "n/a"))

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    expected = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
    analyze(sys.argv[1], expected)
