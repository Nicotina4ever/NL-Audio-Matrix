"""Generate a latency-marker WAV: silence, then a sharp 1 kHz burst from sample 0.
Usage: python make_click_wav.py out.wav [sample_rate] [seconds] [amplitude]
The onset sample is 0 so a recorder can measure the end-to-end delay.
"""
import sys, wave, struct, math
out = sys.argv[1]
sr = int(sys.argv[2]) if len(sys.argv) > 2 else 48000
seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 3.0
amp = float(sys.argv[4]) if len(sys.argv) > 4 else 0.85
n = int(sr * seconds)
frames = bytearray()
burst = int(sr * 0.010)  # 10 ms burst at onset
for i in range(n):
    if i < burst:
        v = amp * math.sin(2.0 * math.pi * 1000.0 * i / sr)
        # hard onset: first sample already near peak after one quarter cycle -> use cosine for instant peak
        v = amp * math.cos(2.0 * math.pi * 1000.0 * i / sr)
    else:
        v = 0.0
    frames += struct.pack('<h', max(-32768, min(32767, int(v * 32767))))
w = wave.open(out, 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(bytes(frames)); w.close()
print(f"wrote {out} sr={sr} frames={n} onset_sample=0")
