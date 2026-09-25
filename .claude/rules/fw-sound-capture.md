---
paths:
  - "fw/src/sound/**"
  - "fw/src/bluetooth/capture_service.cpp"
  - "fw/src/bluetooth/audio_telemetry_service.cpp"
  - "fw/tests/sound/**"
  - "fw/tools/beat_lab/**"
  - "fw/tools/capture_to_scenario.py"
  - "fw/docs/beat-detection-debugging.md"
---

# Audio, beat detection and captures

Loads when you read sound/capture code. Recording a capture as a sim scenario is
`/capture-scenario`.

`fw/src/sound/sound.cpp` — PDM microphone capture + AGC + audio DSP thread; compiled with
`CONFIG_AUDIO`. (The VM3011 driver path is compiled out on proto0 — `CONFIG_VM3011` unset.) Beat
detection lives in the BT-free `fw/src/sound/audio_dsp.cpp`.

## Debugging/tuning beat detection

See `fw/docs/beat-detection-debugging.md` — on-device capture (`sound mic record_wav`, `sound dump`,
`sound agc freeze/gain`, `sound dsp`), a native_sim WAV-replay harness
(`fw/tests/sound/audio_dsp_replay/`), and offline scoring/plot tooling (`fw/tools/beat_lab/`). The
firmware-side pieces are gated by `CONFIG_APP_AUDIO_DEBUG`, which is **`default n`** — it costs
33,440 B of RAM (92.04% → 84.62% of the appcore region when measured), so it is opt-in per debugging
session: rebuild with `-DCONFIG_APP_AUDIO_DEBUG=y`, remembering that a changed Kconfig *default*
needs `rm fw/build/fw/zephyr/.config` or `--pristine` (`.claude/rules/fw-kconfig-build.md`).
`sound dsp set`, `sound agc status|gate`, and `record_wav`'s raw fallback all work without it.
**`sound agc gain`/`freeze`, `sound dump`, and therefore the serial plugin's `sound_record`,
`sound_dump` and `capture_scenario` tools need a `CONFIG_APP_AUDIO_DEBUG` build.**

## Capture file layout

A capture (shell `capture start`, or the app's Capture screen) writes **two** files under `/NAND:`:
`cap_NNNN.wav` and `cap_NNNN.wav.csv`. The second is ONE combined sidecar — IMU `I,` rows
interleaved with per-frame `D,` analysis rows (AGC gain / beat mask / spectrogram, same
`audio_tap_format.h` wire format as `sound dump`) — gated by `CONFIG_APP_CAPTURE_AUDIO_SIDECAR`
(`default y`, `depends on APP_CAPTURE && !APP_AUDIO_DEBUG`, since a debug build routes to
`record_wav_tap()` and never reaches this path). It is one file rather than two because
`FF_FS_TINY` makes every open FIL share one sector window, so a capture holds two FatFs handles
instead of three.

One caveat, because it is easy to state too absolutely: that layout is what `record_wav_capture()`
writes, and `sound_record_wav()` only routes there while the DSP thread is streaming. If it is not
(boot-failure diagnosis), a stock build falls through to `record_wav_direct()`, which writes
`cap_NNNN.wav` + `cap_NNNN.wav.imu.csv` and **no** `.csv` — the older split layout, on a stock
image. `fw/tools/capture_to_scenario.py` picks between them by content rather than name for exactly
this reason.

The analysis exists because the capture path deliberately does **not** freeze the AGC: the gain
steps mid-recording and the samples already contain those steps, so a host cannot re-derive from
the WAV what the device actually saw. It costs ~6 KB RAM and ~11 KB/s of volume, which is why the
longest capture the 6.9 MiB volume holds is ~160 s rather than the 180 s cap; `kBytesPerSecond` in
`fw/src/sound/capture.cpp` is the single place that figure is derived from, and the app's Remaining
S readout reads through it. IMU sidecar rows are `I,ms,seq,ax,ay,az,gx,gy,gz` at 25 Hz, scaled ×1000
(mm/s², mrad/s) — using them to validate the IMU axes:
`.claude/skills/capture-scenario/references/imu-frame-validation.md`.

## No float printf

Print floats via integer fixed-point — `fmt_fixed4()` / `agc_gain_db10()` in `sound.cpp`
(`.claude/rules/fw-logging.md`).
