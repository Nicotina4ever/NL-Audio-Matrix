# Focusrite manual validation — required physical step

The VM now sees:

- Windows: `Scarlett Solo USB` (`USB\VID_1235&PID_8205`)
- ASIO: `Focusrite USB ASIO`
- Inputs: `Input 1`, `Input 2`
- Outputs: `Output 1`, `Output 2`

The router has already opened the device and recorded NL Mic 1, but the
capture contains only the noise floor (RMS ≈ 6.1e-05). That is **not** a real
microphone recording.

## Required user action (about 30 seconds)

1. Connect a microphone to **Input 1** (or an instrument to Input 2) and set a
   safe gain; do not enable Direct Monitor for this test.
2. In the VM, open `C:\NLMatrix\NLMatrixUI.exe`.
3. Load `C:\NLMatrix\focusrite_gui.json` if it is not already loaded.
4. Click **Start**.
5. Speak normally / play the instrument for 15–20 seconds.
6. Tell the agent when you are done; the agent will stop the engine and analyze
   the recorded WAV.

## Capture command (already prepared)

```bat
C:\NLMatrix\NLMatrixDriverAudioTest.exe --record --input "NL Mic 1" ^
  --out C:\NLMatrix\focusrite_mic_test.wav --seconds 20
```

Start this command immediately before speaking, or ask the agent to run it
while you perform the action.

## What will be checked

- input and destination meters;
- WAV non-silence, RMS and peak;
- no clipping introduced by the router at 0 dB;
- continuity / duplicated blocks;
- gain and mute changes;
- Focusrite physical output monitoring, separate from Direct Monitor.
