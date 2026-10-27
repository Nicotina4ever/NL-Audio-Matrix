"""Measure engine end-to-end latency from a test-source marker recording.

Inputs:
  - WAV recorded from NL Mic X (mono PCM16) containing a 1 kHz tone plus short
    2 kHz marker bursts emitted by the router test source;
  - the router log containing lines:  MARKER <src> index=N frame=... wall_ms=T
  - the recorder log containing: ... start_ms=T
The recorder start_ms is taken at audioDeviceAboutToStart; WAV sample k was
captured at approximately start_ms + k/sampleRate*1000. Latency for marker N is
then (capture_time - marker_wall_ms). Both timestamps are JUCE millisecond
counters (QPC on Windows), i.e. the same machine clock across processes.

Usage: python measure_engine_latency.py --wav rec.wav --router router.log --rec rec.log [--json out.json]
"""
import argparse, json, re, sys, wave
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


def find_marker_starts(x, sr, threshold=0.08, min_gap=0.15):
    d = np.abs(np.diff(x, prepend=x[0]))
    above = d > threshold
    starts = []
    last = -1e9
    i = 0
    n = len(above)
    while i < n:
        if above[i]:
            j = i
            while j < n and above[j]:
                j += 1
            t = i / sr
            if t - last > min_gap:
                starts.append(i)
                last = t
            i = j
        else:
            i += 1
    return starts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--wav', required=True)
    ap.add_argument('--router', required=True)
    ap.add_argument('--rec', required=True)
    ap.add_argument('--json', default='')
    a = ap.parse_args()
    x, sr = load_wav(a.wav)
    rec_ms = None
    first_cb_ms = None
    first_cb_samples = 0
    for line in open(a.rec, encoding='utf-8', errors='replace'):
        m = re.search(r'start_ms=([0-9.eE+-]+)', line)
        if m:
            rec_ms = float(m.group(1))
        m = re.search(r'first_cb_ms=([0-9.eE+-]+)', line)
        if m:
            first_cb_ms = float(m.group(1))
        m = re.search(r'first_cb_samples=(\d+)', line)
        if m:
            first_cb_samples = int(m.group(1))
    if rec_ms is None:
        raise SystemExit('recorder start_ms not found')
    # Two time bases for WAV sample 0:
    #   start_base: aboutToStart() wall clock (old behaviour, tends to be early).
    #   first_cb_base: first capture packet timestamp minus its duration; this is
    #                  the more accurate mapping when the new recorder is used.
    sample0_start_ms = rec_ms
    sample0_firstcb_ms = (first_cb_ms - (first_cb_samples / sr) * 1000.0) if first_cb_ms is not None else None
    markers = []
    for line in open(a.router, encoding='utf-8', errors='replace'):
        m = re.match(r'MARKER\s+(\S+)\s+index=(\d+)\s+frame=(\d+)\s+wall_ms=([0-9.eE+-]+)', line.strip())
        if m:
            markers.append({'index': int(m.group(2)), 'frame': int(m.group(3)), 'wall_ms': float(m.group(4))})
    starts = find_marker_starts(x, sr)
    result = {'wav': a.wav, 'sample_rate': sr, 'frames': len(x), 'rec_start_ms': rec_ms,
              'first_cb_ms': first_cb_ms, 'first_cb_samples': first_cb_samples,
              'sample0_start_ms': sample0_start_ms,
              'sample0_firstcb_ms': sample0_firstcb_ms,
              'router_markers': markers, 'detected_marker_starts': starts, 'latency_ms': []}

    # Marker-to-burst pairing: a marker burst can be missed (onset ramp,
    # threshold) so pairing detected bursts by ordinal is unsafe.  Try every
    # consistent offset and keep the one whose latencies are physically
    # plausible and tight; report the offset explicitly.
    def latencies_for_offset(offset):
        # A burst at the very start may be suppressed by the destination
        # connect fade, and a burst generated at the router stop boundary may
        # be detected before its MARKER line is printed: allow unmatched
        # trailing bursts instead of discarding the whole pairing.
        out = []
        for k, s in enumerate(starts):
            if k + offset >= len(markers):
                break
            capture_start_ms = sample0_start_ms + (s / sr) * 1000.0
            out.append((k + offset, int(s), capture_start_ms,
                        markers[k + offset]['wall_ms'],
                        capture_start_ms - markers[k + offset]['wall_ms']))
        return out

    best_offset = None
    best_score = None
    max_offset = max(0, len(markers) - 1)
    for offset in range(0, max_offset + 1):
        rows = latencies_for_offset(offset)
        if not rows:
            continue
        if len(rows) < 3:
            continue
        lats = [r[4] for r in rows]
        plausible = all(-5.0 <= v <= 500.0 for v in lats)
        if not plausible:
            continue
        spread = max(lats) - min(lats)
        unmatched = len(starts) - len(rows)
        score = spread + unmatched * 2.0
        if best_score is None or score < best_score:
            best_score = score
            best_offset = offset
    if best_offset is None:
        best_offset = 0
        print('WARNING: no physically plausible marker pairing; using offset 0', file=sys.stderr)
    result['pairing_offset'] = best_offset
    rows = latencies_for_offset(best_offset) or []
    for marker_index, start_sample, capture_start_ms, generated_ms, lat in rows:
        capture_firstcb_ms = ((sample0_firstcb_ms + (start_sample / sr) * 1000.0)
                              if sample0_firstcb_ms is not None else None)
        entry = {'marker': marker_index, 'start_sample': start_sample,
                 'capture_ms': round(capture_start_ms, 3),
                 'generated_ms': generated_ms,
                 'latency_ms': round(lat, 3)}
        if capture_firstcb_ms is not None:
            entry['latency_firstcb_ms'] = round(capture_firstcb_ms - generated_ms, 3)
        result['latency_ms'].append(entry)
    lats = [e['latency_ms'] for e in result['latency_ms']]
    if lats:
        result['summary'] = {'count': len(lats), 'min_ms': min(lats),
                             'median_ms': float(np.median(lats)), 'max_ms': max(lats)}
    lats2 = [e['latency_firstcb_ms'] for e in result['latency_ms'] if 'latency_firstcb_ms' in e]
    if lats2:
        result['summary_firstcb'] = {'count': len(lats2), 'min_ms': min(lats2),
                                     'median_ms': float(np.median(lats2)), 'max_ms': max(lats2)}
    print(json.dumps(result, indent=1))
    if a.json:
        open(a.json, 'w', encoding='utf-8').write(json.dumps(result, indent=1))

if __name__ == '__main__':
    main()