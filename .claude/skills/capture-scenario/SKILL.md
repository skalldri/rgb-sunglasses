---
name: capture-scenario
description: Record a real audio + IMU capture on the dev board and turn it into a simulator scenario. Use when a synthetic scenario (metronome, sine, ramp) is not a faithful enough stimulus — tuning motion/audio-driven animations, or expanding the beat-detection corpus for issue #264.
allowed-tools: Bash, Read, Write, Edit, mcp__serial
---

# Record a scenario on-device

Every scenario in `fw/sim/scenarios/` is synthetic. That is fine for pinning a
behaviour, and bad for *discovering* one: a synthetic fixture can only be as right as
the author's mental model of the signal. Two committed fixtures turned out to stimulate
something other than what their names promised — a bob phase-locked to the click track
it was supposed to be separable from, and a "head tilt" that swept through 0.82 g, which
any orientation gate would reject. A recording cannot be wrong that way.

The output is a normal scenario file, so everything downstream — `rgbx-sim run`,
goldens, the CI smoke list — works on it unchanged.

## Preconditions

- **Hold the `board` lock.** Serial + USB mass storage, so a conflicting agent would
  corrupt the capture or the FAT volume:
  ```
  Monitor(command: "scripts/hw-lock.sh hold board", persistent: true)
  ```
  ```bash
  timeout 15 bash -c 'until scripts/hw-lock.sh check board >/dev/null 2>&1; do sleep 0.5; done'
  ```
- **Firmware built from a tree containing the IMU sidecar** (`imu_tap_q` in
  `fw/src/imu/imu.cpp`). Older firmware records the WAV but no `.imu.csv`, and the
  capture silently comes out audio-only.
- **Disk space.** The WAV is ~32 KB/s, the IMU sidecar ~1.4 KB/s and the analysis
  sidecar ~11 KB/s into a 6.9 MiB volume, so **~160 s is the hard ceiling** and 20–30 s
  is the useful range. `record_wav` fails early if the volume cannot hold the capture,
  and `capture status` reports the recordable seconds left. Building with
  `CONFIG_APP_CAPTURE_AUDIO_SIDECAR=n` drops the analysis file and puts the ceiling
  back near 180 s.

## 1. Record

`CONFIG_IMU` and the mic both stay live; the same loop writes both files, timestamped
from one `t0`, which is why they need no host-side alignment.

```
mcp__serial__rgb_sunglasses_capture_scenario(
    connection_id=<id>, duration_s=20, name="bob_120bpm")
```

Play the music and do the movement **during** those seconds — the whole point is that
the stimulus is real. Returns `wav_path`, `imu_csv_path`, `imu_samples`, and —
when the firmware wrote one — `analysis_csv_path` + `analysis_frames`.

**Away from a laptop, use the companion app instead.** Controls → Capture starts and stops
the same worker over BLE (`capture start`/`capture stop` and the GATT Capture service are two
front ends onto one implementation), so a capture can be taken in the field and collected
later. The phone never downloads anything — captures accumulate on `/NAND:` as
`cap_NNNN.wav` + `.csv` (one combined sidecar) and step 2 is unchanged. The app's Length picker
writes the persisted limit; the device clamps it to what the volume can still hold and the
screen shows the clamped figure before you start. Note the app path does **not** freeze AGC
gain the way the MCP tool does — for a reproducible stimulus, prefer the tool. What the app
path *does* give you is `<wav>.csv`, the per-frame analysis (gain, beats, spectrogram)
the DSP computed for those samples, which is the only way to reconstruct what the detector
saw across a live-AGC recording — see `fw/docs/beat-detection-debugging.md`.

**Check `imu_samples` is roughly `25 × duration_s`.** Zero means the sidecar never
opened (firmware without `CONFIG_IMU`, or a full disk) and the scenario would be
audio-only without saying so loudly.

The tool freezes AGC gain for the capture so the stimulus is reproducible, and
**always unfreezes afterwards** — a pinned AGC would silently kill gain adaptation, and
every beat-reactive animation with it, for the rest of the board's uptime. Confirm
`agc_restored: true` in the result.

## 2. Fetch

Same mount/copy/umount pattern as `fw/scripts/coredump-fetch.sh`. Identify the disk by
its SCSI string, never a fixed `/dev/sdX` — the letter moves.

```bash
dmesg | grep -A2 "RGB-SG"          # find it
lsblk                              # cross-check the ~6.9 MiB size
mkdir -p /mnt/sunglasses-fs && mount -o ro /dev/sdX /mnt/sunglasses-fs
cp /mnt/sunglasses-fs/bob_120bpm.wav* <dest>/
umount /mnt/sunglasses-fs && rmdir /mnt/sunglasses-fs
```

Mount **read-only**: the firmware still has the volume mounted, and a host-side write
against a mounted FAT is what corrupts it (see `fw/CLAUDE.md`, "FAT concurrent access").

## 3. Convert

```bash
python3 fw/tools/capture_to_scenario.py <dest>/bob_120bpm.wav \
    --description "Head bobbing to 120 BPM house, upright, ~20 s." \
    --beat-response
```

Writes `fw/sim/scenarios/bob_120bpm.json` and `fw/sim/scenarios/assets/bob_120bpm.wav`.

- **Write a real `--description`.** It is the only record of what physically happened;
  the default is a placeholder that says nothing about the movement.
- `--beat-response` only if the capture actually contains music — it asserts lit pixels
  on beat ticks exceed off-beat ticks, and would fail forever on a silent capture.
- `--hz` controls IMU keyframe decimation (default 12.5 from the native 25). Use
  `--hz 0` to keep every sample when the motion detail matters.

## 4. Verify

```bash
cd fw/sim && ./rgbx-sim scenarios | grep bob_120bpm
./rgbx-sim run out/wasm/<ext>.wasm --scenario bob_120bpm --no-build
```

Expect `PASS no_fault` and, for a music capture, `PASS beat_response` — that check is
what proves the recorded audio is genuinely reaching the DSP rather than the scenario
merely loading.

To watch it instead of reading digests, the browser sim replays it too
(`npm run serve` → Inputs tab → Scenario). A committed capture appears in the
scenario picker automatically; an uncommitted one loads via "Load scenario…" —
pick the `.json` together with its `assets/*.wav` in one multi-select (refs
resolve by basename).

## 5. Decide whether it earns a place in the repo

A capture is only worth committing if it shows something the synthetic set cannot. Say
what that is in the description. The WAV is a binary asset in a repo that otherwise
generates its assets from scripts, so a 20 s capture (~640 KB) needs a reason.

If it is worth keeping, consider adding it to `GOLDEN_SPECS` in `fw/sim/node/golden.ts`
and the smoke list in `.github/workflows/sim-ci.yml`, so it is re-run rather than only
executed when someone types its name.

## Pitfalls

- **The board must be worn or moved for the IMU track to mean anything.** A capture
  taken with the glasses flat on a desk has a valid IMU track that is a constant
  gravity vector — which looks fine in the JSON and teaches nothing.
- **`sound dump` is a different tool.** It streams the same analysis D-lines this
  capture now writes to `<wav>.csv`, but live to the console, with no audio and
  no IMU, and only on a `CONFIG_APP_AUDIO_DEBUG=y` build
  (`fw/docs/beat-detection-debugging.md`). Use it to watch the detector while you turn
  a knob; use this when you need a stimulus you can replay.
- **Do not reformat or write to the NAND disk while the firmware has it mounted.**
- After fetching, reset the board if you also *wrote* to the disk — the firmware's
  cached FAT will not see host-side changes.
