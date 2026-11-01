#!/usr/bin/env python3
"""Deterministic level/tone/continuity measurements for recorded WAV files.

Used by tools/vm/Run-SendCableCheck.ps1 on the HOST: the guest only records,
the host analyzes the WAV (the product rule keeps audio I/O in the VM).

Measures, per file:
  * mean and peak level in dBFS;
  * Goertzel magnitude at a target frequency per channel (default 1000 Hz L,
    1040 Hz R), reported in dBFS and as a tone/noise ratio;
  * continuity: longest run of near-silent samples (|x| < threshold) and the
    silent-sample ratio, so a dropout is visible without listening.

No timestamps, no randomness: the same WAV always yields the same JSON.
"""

import argparse
import json
import math
import struct
import sys
import wave


def read_wav(path):
    with wave.open(path, "rb") as handle:
        channels = handle.getnchannels()
        width = handle.getsampwidth()
        rate = handle.getframerate()
        frames = handle.getnframes()
        raw = handle.readframes(frames)
    if width != 2:
        raise SystemExit("only 16-bit PCM is supported: %s (%d bytes/sample)" % (path, width))
    count = frames * channels
    values = struct.unpack("<%dh" % count, raw[: count * 2])
    data = [[0.0] * frames for _ in range(channels)]
    for index in range(frames):
        base = index * channels
        for channel in range(channels):
            data[channel][index] = values[base + channel] / 32768.0
    return rate, channels, data


def dbfs(value):
    if value <= 0.0:
        return -120.0
    return 20.0 * math.log10(value)


def goertzel(samples, rate, frequency):
    """Normalized amplitude (0..1 full scale) of one frequency."""
    count = len(samples)
    if count == 0:
        return 0.0
    coefficient = 2.0 * math.cos(2.0 * math.pi * frequency / rate)
    s1 = 0.0
    s2 = 0.0
    for sample in samples:
        s0 = sample + coefficient * s1 - s2
        s2 = s1
        s1 = s0
    power = s1 * s1 + s2 * s2 - coefficient * s1 * s2
    if power < 0.0:
        power = 0.0
    return 2.0 * math.sqrt(power) / count


def analyze(path, tone_left, tone_right, silence_threshold, warmup_ms, silence_channel=-1):
    rate, channels, data = read_wav(path)
    total = 0.0
    peak = 0.0
    count = 0
    longest_silence = 0.0
    longest_silence_after_warmup = 0.0
    silent = 0
    warmup_frames = int(rate * warmup_ms / 1000.0) if rate else 0
    for channel_index, channel in enumerate(data):
        run = 0
        for index, sample in enumerate(channel):
            total += sample * sample
            if abs(sample) > peak:
                peak = abs(sample)
            if abs(sample) < silence_threshold and (silence_channel < 0 or channel_index == silence_channel):
                run += 1
                silent += 1
                if run > longest_silence:
                    longest_silence = run
                if index >= warmup_frames and run > longest_silence_after_warmup:
                    longest_silence_after_warmup = run
            else:
                run = 0
            count += 1
    # Interior continuity: silence runs between the first and last non-silent
    # sample (leading/trailing silence is a recording-window artefact, not a dropout).
    interior_channel = silence_channel if (0 <= silence_channel < channels) else 0
    samples_of_interest = data[interior_channel] if data else []
    first_audio = None
    last_audio = None
    for index, sample in enumerate(samples_of_interest):
        if abs(sample) >= silence_threshold:
            if first_audio is None:
                first_audio = index
            last_audio = index
    longest_interior = 0
    if first_audio is not None and last_audio is not None and last_audio > first_audio:
        run = 0
        for index in range(first_audio, last_audio + 1):
            if abs(samples_of_interest[index]) < silence_threshold:
                run += 1
                if run > longest_interior:
                    longest_interior = run
            else:
                run = 0
    rms = math.sqrt(total / count) if count else 0.0
    left = data[0] if channels > 0 else []
    right = data[1] if channels > 1 else left
    band_left = goertzel(left, rate, tone_left)
    band_right = goertzel(right, rate, tone_right)
    # The opposite channel at the same frequency is the tone leakage/noise probe.
    leakage_left = goertzel(left, rate, tone_right)
    leakage_right = goertzel(right, rate, tone_left)
    return {
        "file": path,
        "sampleRate": rate,
        "channels": channels,
        "frames": len(left),
        "seconds": round(len(left) / float(rate), 3) if rate else 0.0,
        "meanDb": round(dbfs(rms), 2),
        "peakDb": round(dbfs(peak), 2),
        "toneLeftDb": round(dbfs(band_left), 2),
        "toneRightDb": round(dbfs(band_right), 2),
        "leakLeftDb": round(dbfs(leakage_left), 2),
        "leakRightDb": round(dbfs(leakage_right), 2),
        "toneRatioLeft": round(band_left / leakage_left, 3) if leakage_left > 0 else 999.0,
        "toneRatioRight": round(band_right / leakage_right, 3) if leakage_right > 0 else 999.0,
        "longestSilenceMs": round(1000.0 * longest_silence / float(rate), 2) if rate else 0.0,
        "longestSilenceAfterWarmupMs": round(1000.0 * longest_silence_after_warmup / float(rate), 2) if rate else 0.0,
        "longestInteriorSilenceMs": round(1000.0 * longest_interior / float(rate), 2) if rate else 0.0,
        "warmupMs": warmup_ms,
        "silentRatio": round(silent / float(count), 4) if count else 1.0,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description="Level/tone/continuity measurements for WAV files")
    parser.add_argument("wav", nargs="+", help="WAV file(s) to analyze")
    parser.add_argument("--tone-left", type=float, default=1000.0)
    parser.add_argument("--tone-right", type=float, default=1040.0)
    parser.add_argument("--silence-threshold", type=float, default=1e-4)
    parser.add_argument("--warmup-ms", type=float, default=200.0,
                        help="ignore silence runs that start in this startup window")
    parser.add_argument("--silence-channel", type=int, default=-1,
                        help="measure continuity on this channel only (-1 = all)")
    parser.add_argument("--json", default=None, help="write the JSON here (default: stdout)")
    args = parser.parse_args(argv)

    results = [analyze(path, args.tone_left, args.tone_right, args.silence_threshold,
                       args.warmup_ms, args.silence_channel)
               for path in args.wav]
    text = json.dumps({"analyser": "analyze_send_path", "version": "1.0.0", "files": results},
                      indent=2, sort_keys=True)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            handle.write(text + "\n")
    else:
        sys.stdout.write(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())



