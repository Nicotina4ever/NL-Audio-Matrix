"""Analyze one capture WAV for the channel-split test.

Reports RMS/peak, dominant FFT frequency, and the amplitude at the expected
tone frequency/frequencies.  This is a software measurement of the recorded
stream (not an analog loopback).  Usage:
  python tools/analyze_channel_split.py --wav file.wav --expect 600
"""
import argparse
import json
import wave

import numpy as np


def load_wav(path):
    w = wave.open(path, 'rb')
    sr = w.getframerate()
    n = w.getnframes()
    ch = w.getnchannels()
    sw = w.getsampwidth()
    raw = w.readframes(n)
    w.close()
    if sw == 2:
        x = np.frombuffer(raw, dtype='<i2').astype(np.float64) / 32768.0
    elif sw == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        x = v.astype(np.float64) / (1 << 23)
    elif sw == 4:
        x = np.frombuffer(raw, dtype='<i4').astype(np.float64) / (1 << 31)
    else:
        raise SystemExit(f'unsupported sample width {sw}')
    if ch > 1:
        x = x.reshape(-1, ch)[:, 0]
    return x, sr


def goertzel_rms(x, sr, freq):
    n = len(x)
    if n == 0:
        return 0.0
    t = np.arange(n) / sr
    c = np.cos(2.0 * np.pi * freq * t)
    s = np.sin(2.0 * np.pi * freq * t)
    re = float(np.dot(x, c))
    im = float(np.dot(x, s))
    return float(np.sqrt((re * re + im * im)) * 2.0 / n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--wav', required=True)
    ap.add_argument('--expect', default='', help='comma separated expected tones, e.g. 600 or 600,900')
    ap.add_argument('--json', default='')
    a = ap.parse_args()
    x, sr = load_wav(a.wav)
    # Discard leading silence so the measurements use the active part only.
    env = np.abs(x)
    onset = 0
    for i in range(0, len(env), max(1, sr // 100)):
        if np.max(env[i:i + sr // 100]) > 1e-3:
            onset = i
            break
    active = x[onset:]
    if len(active) < sr // 10:
        active = x
    freqs = [float(v) for v in a.expect.split(',') if v.strip()]
    result = {
        'wav': a.wav,
        'sample_rate': sr,
        'frames': int(len(x)),
        'onset_s': round(onset / sr, 4),
        'active_s': round(len(active) / sr, 4),
        'rms_dbfs': round(20.0 * np.log10(float(np.sqrt(np.mean(active * active))) + 1e-12), 3),
        'peak_dbfs': round(20.0 * np.log10(float(np.max(np.abs(active))) + 1e-12), 3),
        'silent': bool(np.max(np.abs(active)) < 1e-4),
        'freqs': {},
    }
    if len(active) > 256:
        spec = np.abs(np.fft.rfft(active * np.hanning(len(active))))
        df = sr / len(active)
        k0 = max(1, int(50.0 / df))
        k1 = min(len(spec) - 1, int(5000.0 / df))
        k = k0 + int(np.argmax(spec[k0:k1 + 1]))
        result['dominant_hz'] = round(k * df, 3)
    for f in freqs:
        amp = goertzel_rms(active, sr, f)
        result['freqs'][str(f)] = {
            'rms_dbfs': round(20.0 * np.log10(amp + 1e-12), 3),
            'relative_to_peak_db': round(20.0 * np.log10(amp / (np.max(np.abs(active)) + 1e-12)), 3),
        }
    print(json.dumps(result, indent=1))
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as h:
            h.write(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
