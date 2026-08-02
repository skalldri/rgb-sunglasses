# Beat-detection debugging environment (issue #264)

Tooling for diagnosing and tuning the beat detector (`fw/src/sound/audio_dsp.cpp`)
and AGC (`fw/src/sound/sound.cpp`) quantitatively instead of by eye. The core
idea: record real audio **and** the exact per-frame analysis the firmware
computed for it, replay the same audio through the identical DSP code on the
host at build-of-a-few-seconds iteration speed, and score detections against a
reference with a MIREX-standard F-measure.

Everything firmware-side is gated by `CONFIG_APP_AUDIO_DEBUG` (default y on
proto0; disable to reclaim ~33 KB of static RAM — 16-entry tap queue ~18.5 KB
plus WAV/CSV batching and drain buffers ~14.5 KB — once tuning is done).

## Pieces

| Piece | Where | What |
|---|---|---|
| Audio tap | `sound.cpp` (`audio_tap_q`) | DSP thread tees each 512-sample PCM block + `audio_analysis_result` into a 16-deep queue when armed; `record_wav`/`dump` drain it. The DSP thread stays the only `dmic_read()` consumer. |
| `sound mic record_wav [s] [path]` | shell | Writes WAV (+ sidecar `.csv` with one D-line per frame) to `/NAND:` from the tap. Sector-aligned batched writes (JUNK-chunk-padded WAV header) — a 30 s capture runs at 0 dropped frames; transient QSPI errors are retried (close/reopen clears FatFS's sticky error flag). 180 s hard cap + an upfront free-space check; 30–60 s is the working size. An aborted capture prints `ABORTED: capture incomplete` and returns an error (the MCP tool reports `record_failed`). When the DSP thread isn't streaming (boot failure diagnosis) — or with `CONFIG_APP_AUDIO_DEBUG=n` — it falls back to a direct raw capture (WAV only, no sidecar), so mic capture exists in every build. |
| `sound dump <frames> [buckets]` | shell | Streams live D-lines to the console (no MSC roundtrip). |
| `sound agc freeze [on\|off]` / `sound agc gain <0..0x50>` | shell | Freeze AGC / set PDM gain directly (gain implies freeze). **Record with frozen gain** — a mid-capture gain step makes device-vs-host comparison impossible. |
| `sound dsp params` / `sound dsp set <gamma\|floor\|alpha\|refractory> <v>` | shell | Read/write the detector parameters (same values as the BLE `audio/` characteristics; persisted). |
| Replay harness | `fw/tests/sound/audio_dsp_replay/` | native_sim app compiling the **real** `audio_dsp.cpp`; env-driven (`BEAT_WAV`, `BEAT_GAMMA/ALPHA/FLOOR/REFRACTORY`, `BEAT_AGC=off\|sim`, `BEAT_GAIN`, `BEAT_OUT`, `BEAT_BUCKETS`). Its Twister scenario self-tests in CI. |
| Python tooling | `fw/tools/beat_lab/` | `frames.py` (codec), `replay.py` (build+run, `--sweep` grids), `compare.py` (device-vs-host gate), `evaluate.py` (P/R/F vs annotations or librosa), `report.py` (diagnostic plot). Tests: `fw/tools/tests/test_beat_lab.py`. |
| Serial MCP tools | `.serial_mcp/plugins/rgb_sunglasses.py` | `rgb_sunglasses.sound_record` (freeze gain → record → parsed result) and `rgb_sunglasses.sound_dump` (capture N frames to a host file). |

The D-line wire format is documented in `fw/tools/beat_lab/frames.py`; producers
are `tap_frame_format()` in `sound.cpp` and the replay app — keep all three in
sync. Floats travel as 8-hex-char IEEE-754 bit patterns (exact, `%f`-free).

## The workflow

```bash
# 1. On-device capture (shell, or the rgb_sunglasses.sound_record MCP tool):
sound agc gain 0x28          # frozen, known gain (pick a level that doesn't clip)
sound mic record_wav 30      # play music near the glasses meanwhile
#    → /NAND:/sound.wav + /NAND:/sound.wav.csv ("... 0 dropped, 0 io retries")

# 2. Pull both files off the USB mass-storage disk (see fw/CLAUDE.md "USB Flash
#    Disk" for mount procedure; read-only mount, umount when done).

# 3. Host replay of the same WAV through the same DSP code:
python3 fw/tools/beat_lab/replay.py --wav sound.wav --gain 0x28 --out host.txt

# 4. Validate the replica reproduces the device (the acceptance gate):
python3 -m tools.beat_lab.compare --device sound.wav.csv --host host.txt   # from fw/
#    → PASS = tuning results from the harness transfer to hardware.

# 5. Score against a reference and keep the JSON as the baseline:
python3 -m tools.beat_lab.evaluate --frames host.txt --wav sound.wav \
    --ref-librosa beats --json > baseline.json

# 6. Visualize what the detector saw:
python3 -m tools.beat_lab.report --frames host.txt --wav sound.wav \
    --ref-librosa beats --out report.png

# 7. Tune: parameter sweeps need no rebuild (env-driven), code edits rebuild in
#    seconds:
python3 fw/tools/beat_lab/replay.py --wav sound.wav \
    --sweep "alpha=2.0:5.0:0.5,floor=0.005:0.05:0.005" --ref-librosa beats

# 8. Spot-check the winner on hardware:
sound dsp set alpha 2.5      # etc. — persisted via the settings subsystem
sound dump 300               # watch beats live while music plays
```

Ground-truth options for step 5: `--ref-librosa beats` (librosa beat tracker),
`--ref-librosa onsets`, or `--ref annotations.txt` (one time-in-seconds per
line; Audacity label-track exports work).

## Interpreting compare.py

- Frames are paired **positionally** (device CSV row k ↔ host replay frame k):
  both describe the k-th 512-sample block of the same WAV. Likewise all
  `FrameDump.times` are capture-index based (the WAV timeline) — the device seq
  counter keeps counting across dropped frames, so seq is used only to *detect*
  gaps, never to align or timestamp. A gapped capture's timeline is compressed
  vs wall clock by 32 ms per dropped frame.
- The first 32 frames (and 32 frames after any seq gap) are excluded from
  beat-mask scoring: the device's detector history is warm when the tap arms,
  the host replica starts cold.
- Cortex-M33 FMA vs host SSE means fields match to ~1e-7 absolute, not
  bit-exactly; beat flags on razor-thin threshold margins can flip. `compare.py`
  prints the margin for every mismatch — flips at |margin| ≈ 0 are noise, flips
  at large margins mean the harness has drifted from the firmware.
- Validated 2026-08-01 on a real 30 s capture: 937/937 frames aligned, 100%
  beat-mask agreement on scored frames, max |Δ| ≈ 5e-9 (energy) / 5e-7 (flux).

## Known pitfalls

- **Record at frozen gain** (`sound agc gain`) or `BEAT_AGC=sim` comparison is
  meaningless — the recorded samples already contain the hardware gain steps.
- The recorded WAV is post-PDM-gain: at 0 dB in a quiet room expect tiny RMS
  (~2e-4). That's fine for the detector (log-domain flux) but normalize before
  listening to the WAV.
- Params snapshot: the `#PARAMS` line captures gamma/alpha/floor/refractory at
  arm time; changing them (BLE/shell) mid-capture invalidates the capture.
- `sound dump`/`record_wav` refuse to run concurrently (one tap).
- The PDM stream can die under extreme system load (QSPI + USB MSC + BLE); the
  DSP thread self-heals within ~200 ms (`PDM stream appears dead; attempting
  restart` in the log) and resets detector history when it does.
- Changing `CONFIG_APP_AUDIO_DEBUG`'s default doesn't take effect on an
  incremental build — see fw/CLAUDE.md "An incremental build IGNORES a changed
  Kconfig default".
