# Beat-detection debugging environment (issue #264)

Tooling for diagnosing and tuning the beat detector (`fw/src/sound/audio_dsp.cpp`)
and AGC (`fw/src/sound/sound.cpp`) quantitatively instead of by eye. The core
idea: record real audio **and** the exact per-frame analysis the firmware
computed for it, replay the same audio through the identical DSP code on the
host at build-of-a-few-seconds iteration speed, and score detections against a
reference with a MIREX-standard F-measure.

## Enabling the rig — it is OFF by default

The firmware-side measurement surface is gated by `CONFIG_APP_AUDIO_DEBUG`,
which is **`default n`**. It is the largest reclaimable block of static RAM in
the application image — measured on the linker's region table, not estimated:

| | `=y` | `=n` (shipped) | Δ |
|---|---|---|---|
| appcore RAM | 414,688 B (92.04%) | 381,248 B (84.62%) | **−33,440 B / −7.42 pts** |
| appcore FLASH | 742,236 B (82.41%) | 738,776 B (82.03%) | −3,460 B |

Turn it on for a debugging session:

```bash
west build --build-dir fw/build fw --board rgb_sunglasses_proto0/nrf5340/cpuapp \
    --sysbuild -- -DBOARD_ROOT="$(pwd)/fw" -DCONFIG_APP_AUDIO_DEBUG=y
```

**A Kconfig `default` change is ignored by an incremental build** (see
fw/CLAUDE.md), so flipping this against an existing `fw/build` needs
`rm fw/build/fw/zephyr/.config` first, or `--pristine`. Always confirm before
trusting a capture:

```bash
grep CONFIG_APP_AUDIO_DEBUG fw/build/fw/zephyr/include/generated/zephyr/autoconf.h
```

**What still works with it OFF** — the whole tuning surface, just not the
console measurement one: `sound dsp params` / `sound dsp set` (every detector
tunable), `sound agc status|gate|rate|attack|release`, `sound rms`, the BLE
`audio/` characteristics, and `sound mic record_wav` itself. What you lose:
`sound dump`, `sound agc freeze` and `sound agc gain`.

**A per-frame analysis sidecar is NOT one of the things you lose.** A stock
build's capture path writes `<wav>.audio.csv` under
`CONFIG_APP_CAPTURE_AUDIO_SIDECAR` (`default y`), in the same format, so
device-vs-host comparison works on a shipping image. Two differences from the
`=y` sidecar worth knowing: it is named `.audio.csv` rather than `.csv`, and it
always carries the display buckets (41 fields, not 21). What `=y` still buys is
the console tools and the ability to pin the gain — without `sound agc freeze`
you cannot record a *fixed-gain* clip, which is what the corpus below wants.

## Pieces

| Piece | Where | What |
|---|---|---|
| Audio tap | `sound.cpp` (`audio_tap_q`) | DSP thread tees each 512-sample PCM block + `audio_analysis_result` into a 16-deep queue when armed; `record_wav`/`dump` drain it. The DSP thread stays the only `dmic_read()` consumer. |
| `sound mic record_wav [s] [path]` | shell | Writes WAV (+ sidecar `.csv` with one D-line per frame) to `/NAND:` from the tap. Sector-aligned batched writes (JUNK-chunk-padded WAV header) — a 30 s capture runs at 0 dropped frames; transient QSPI errors are retried (close/reopen clears FatFS's sticky error flag). 180 s hard cap + an upfront free-space check; 30–60 s is the working size. An aborted capture prints `ABORTED: capture incomplete` and returns an error (the MCP tool reports `record_failed`). When the DSP thread isn't streaming (boot failure diagnosis) — or with `CONFIG_APP_AUDIO_DEBUG=n` — it falls back to a direct raw capture (WAV only, no sidecar), so mic capture exists in every build. |
| `sound dump <frames> [buckets]` | shell | Streams live D-lines to the console (no MSC roundtrip). |
| Capture analysis sidecar | `sound.cpp` (`audio_sidecar`), `CONFIG_APP_CAPTURE_AUDIO_SIDECAR` (**default y**) | `<wav>.audio.csv` beside every capture on the *stock* build — same `#PARAMS` + `D,` format as `sound dump`, buckets always included (41 fields). Fed by a second small tap (`capture_analysis_q`) drained in lockstep with the PCM one, so rows and WAV blocks share an index. ~6 KB RAM, ~11 KB/s of volume. This is what makes a phone-started capture analysable — see below. |
| `sound agc freeze [on\|off]` / `sound agc gain <0..0x50>` | shell | Freeze AGC / set PDM gain directly (gain implies freeze). **Record with frozen gain** — a mid-capture gain step makes device-vs-host comparison impossible. |
| `sound dsp params` / `sound dsp set <gamma\|floor\|alpha\|refractory\|sf_delta\|mode> <v>` | shell | Read/write the detector parameters (same values as the BLE `audio/` characteristics; persisted). `mode` selects the adaptive-threshold shape: 0 = `mean + alpha*sigma` (default), 1 = `median + sf_delta`. Switching modes takes effect on the next frame, so a live A/B needs no reflash. |
| Replay harness | `fw/tests/sound/audio_dsp_replay/` | native_sim app compiling the **real** `audio_dsp.cpp`; env-driven (`BEAT_WAV`, `BEAT_GAMMA/ALPHA/FLOOR/REFRACTORY`, `BEAT_SF_DELTA`, `BEAT_THRESHOLD_MODE`, `BEAT_AGC=off\|sim`, `BEAT_GAIN`, `BEAT_OUT`, `BEAT_BUCKETS`). Its Twister scenario self-tests in CI. |
| Python tooling | `fw/tools/beat_lab/` | `frames.py` (codec), `replay.py` (build+run, `--sweep` grids), `compare.py` (device-vs-host gate), `evaluate.py` (P/R/F vs annotations or librosa), `report.py` (diagnostic plot), `phase3_table.py` (mode-0 vs mode-1 acceptance table over the whole corpus). Tests: `fw/tools/tests/test_beat_lab.py`. |
| Reference corpus | `fw/testdata/beat-corpus/` | The clips every tuning claim is measured against — see "The corpus" below. |
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

## Recording new corpus material

`/capture-scenario` records audio **and IMU together** on the device (one
`record_wav` loop, one `t0`, so the two streams need no host-side alignment) and
converts the result into a simulator scenario via
`fw/tools/capture_to_scenario.py`. Use it to expand the corpus below — the Phase 5
assessment is explicitly blocked on getting from 3 clips to 6-8, and on the fact
that one of the current three cannot be tempo-tracked at all.

The sidecar it adds (`<wav>.imu.csv`) is a separate file on purpose: `parseDLines`
accepts exactly 21 or 41 fields per `D,` row, so widening the analysis CSV would
break every consumer in this directory. The analysis sidecar
(`<wav>.audio.csv`) is separate for the same reason, and stays inside that 41-field
arity rather than adding columns — which is why the AGC noise-gate `silent` flag
is not in it. Approximate it from the row's RMS against `gate=` in `#PARAMS`.

**A capture started from the phone is now worth analysing.** That path does not
freeze the AGC — a field recording has to take the room as it finds it — so its
gain steps mid-capture, and the recorded samples already contain those steps.
Re-deriving features on the host from the WAV alone therefore cannot reproduce
what the device saw. `<wav>.audio.csv`'s per-frame gain column is the missing
piece; replay with `--params-from` and the gain column, not with a single
`--gain`. `compare.py`'s notion of a PASS still assumes a constant gain, so a
live-AGC capture is for *reading* what the detector did, not for re-validating
the harness — use a frozen-gain bench clip for that.

## The corpus

`fw/testdata/beat-corpus/` holds the clips every tuning claim in issue #264 is
measured against. Recorded 2026-08-02 on proto0, PDM gain frozen at +9 dB
(`0x3a`) for the music clips so device-vs-host replay is exact:

| Clip | Length | Content | librosa ref beats |
|---|---|---|---|
| `base60` | 60 s | EDM, moderate volume — the primary baseline | 126 (~126 BPM) |
| `loud30` | 30 s | same source, volume raised ~7 dB | 63 |
| `newbase` | 30 s | earlier session, +4 dB, different track | 78 (~156 BPM) |
| `quiet40` | 40 s | quiet room, AGC **unfrozen** — captures the park-to-0 dB walk | n/a |

**These clips are NOT in the repo** — `fw/testdata/beat-corpus/` is gitignored
(a 60 s WAV is ~2 MB, and the rig regenerates a corpus in minutes). A fresh
checkout has an empty directory, so the numbers above are not reproducible until
you record your own; `phase3_table.py` checks for the clips up front and points
back here rather than failing deep in a run.

Record each clip with the workflow above, keeping **both** the `.wav` and its
`.wav.csv` sidecar — `--params-from` needs the sidecar to replay with the exact
parameters the device used, and `compare.py` needs it to re-verify the harness.
Any music works; match the table's lengths and levels if you want comparable
numbers, or use `--clips <name>` to score your own set.

**Tune against the whole corpus, never one clip.** A parameter's best value on
a single clip routinely differs from the best SHARED value across clips, and
only the latter is shippable — `phase3_table.py` reports both, plus the
"max regret" of a shared setting versus each clip's own optimum.

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
  - **Exception: large margins are also normal just past the warm-up cutoff.**
    The 32-frame exclusion covers refilling the flux ring, but not the
    *refractory phase*: a warm-up-only fire sets a 5-frame refractory, and each
    resulting difference can set another, so the transient can run several
    frames past 32. Observed 2026-08-02 at alpha=0.3 (verify30): 3 scored flips
    at frames 33/36/40 with margins up to +0.26, device threshold ~6x the
    host's because the device ring held real music while the host's still held
    warm-up zeros — yet agreement from frame 41 onward was 896/896 = 100%.
    Before concluding "drift", check whether ALL differing frames are clustered
    at the start; a genuine drift scatters mismatches through the whole capture.
    Lower alphas make this more visible (a more sensitive threshold reacts more
    to history content).
- Validated 2026-08-01 on a real 30 s capture: 937/937 frames aligned, 100%
  beat-mask agreement on scored frames, max |Δ| ≈ 5e-9 (energy) / 5e-7 (flux).

## Known pitfalls

- **Record at frozen gain** (`sound agc gain`) or `BEAT_AGC=sim` comparison is
  meaningless — the recorded samples already contain the hardware gain steps.
- **A sweep that peaks at the edge of its range has not found the optimum.**
  Issue #264's alpha was swept over 1.0–3.5 and "best at 1.0" was read as a
  result; re-sweeping down to the 0.1 clamp floor moved band-0 F from 0.156 to
  0.294 on the same clip. Always widen the range until the peak is interior, or
  until you hit the parameter's clamp (then say so).
- **Any change to the band bin boundaries invalidates every prior sweep number.**
  Comparing a run from before such a change with one from after is not an A/B —
  it silently confounds the two. Re-measure both arms on one build.
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
