#!/usr/bin/env python3
"""Turn an on-device capture into a runnable simulator scenario.

Every scenario in fw/sim/scenarios/ is synthetic — metronome clicks, sine IMU,
linear ramps. Synthetic fixtures can be wrong in ways nobody notices, because the
only reference is the author's mental model: two committed fixtures turned out to
be stimulating something other than what their names promised (a bob
phase-locked to the click track it was meant to be separable from, and a "head
tilt" that swept through 0.82 g). A recording cannot be wrong that way — the
stimulus is whatever actually happened on a head.

Input is what `sound mic record_wav <secs> <path>` leaves on /NAND::

    <path>            16 kHz mono WAV
    <path>.csv        combined capture CSV: "#PARAMS"/"#IMU" headers, then interleaved
                      I,<ms>,<seq>,ax,ay,az,gx,gy,gz and D,... analysis rows. Only the
                      I-rows are read here; beat_lab reads the D-rows from the same file.
                      (<path>.imu.csv, the older split layout, is still accepted.)

Output is a scenario plus its audio asset:

    fw/sim/scenarios/<name>.json
    fw/sim/scenarios/assets/<name>.wav

The audio goes in as {"type": "wav"}, which the simulator runs through the REAL
DSP (the same wasm build the device runs, gated for parity in CI) rather than
replaying pre-computed decisions — so a scenario stays useful after a DSP change.
The IMU goes in as keyframes, which the simulator interpolates between.
"""

import argparse
import json
import shutil
import struct
import sys
from pathlib import Path

# The sidecar writes scaled integers because the firmware has no float printf
# (CONFIG_CBPRINTF_FP_SUPPORT is off). The header states the scale; this is only
# the fallback for a header-less file.
DEFAULT_SCALE = 1000


def _has_imu_rows(path):
    """True if `path` exists and actually contains at least one I-row.

    Cheap and streaming: a capture CSV is a few hundred KB and the first I-row
    lands within the first couple of rows, so this stops almost immediately.
    """
    try:
        with open(path, "r", errors="replace") as handle:
            return any(line.startswith("I,") for line in handle)
    except OSError:
        return False


def parse_imu_csv(text):
    """Parse the sidecar into [(ms, accel[3], gyro[3])], ordered by time.

    Unknown lines are skipped rather than rejected, the same way parseDLines
    tolerates a mixed stream — the file is written by a device that may also be
    logging, and a stray console line should not throw away a capture.
    """
    scale = DEFAULT_SCALE
    samples = []
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("#IMU"):
            for field in line.split():
                if field.startswith("scale="):
                    scale = int(field.split("=", 1)[1])
            continue
        if not line.startswith("I,"):
            continue
        parts = line.split(",")
        if len(parts) != 9:
            continue
        try:
            values = [int(p) for p in parts[1:]]
        except ValueError:
            continue
        ms = values[0]
        accel = [v / scale for v in values[2:5]]
        gyro = [v / scale for v in values[5:8]]
        samples.append((ms, accel, gyro))
    samples.sort(key=lambda s: s[0])
    return samples


def decimate(samples, hz):
    """Keep roughly `hz` samples per second, always keeping the first and last.

    The IMU runs at 25 Hz, so a 20 s capture is 500 samples — readable as JSON
    but tedious to diff. Halving it loses nothing a keyframe interpolation would
    not reconstruct, and the endpoints are kept so the scenario spans the full
    duration rather than stopping short.
    """
    if not samples or hz <= 0:
        return samples
    step_ms = 1000.0 / hz
    kept = [samples[0]]
    for sample in samples[1:-1]:
        if sample[0] - kept[-1][0] >= step_ms:
            kept.append(sample)
    if len(samples) > 1 and kept[-1][0] != samples[-1][0]:
        kept.append(samples[-1])
    return kept


def wav_duration_ms(path):
    """Duration from the WAV header, without decoding the data."""
    with open(path, "rb") as handle:
        riff = handle.read(12)
        if len(riff) < 12 or riff[0:4] != b"RIFF" or riff[8:12] != b"WAVE":
            raise ValueError(f"{path} is not a RIFF/WAVE file")
        rate = None
        block_align = None
        while True:
            header = handle.read(8)
            if len(header) < 8:
                break
            chunk_id, size = struct.unpack("<4sI", header)
            if chunk_id == b"fmt ":
                fmt = handle.read(size)
                _, _, rate, _, block_align, _ = struct.unpack("<HHIIHH", fmt[:16])
            elif chunk_id == b"data":
                if not rate or not block_align:
                    raise ValueError(f"{path}: data chunk precedes fmt")
                return int(size / block_align / rate * 1000)
            else:
                handle.seek(size + (size & 1), 1)
    raise ValueError(f"{path}: no data chunk")


def build_scenario(name, description, duration_ms, frames, beat_response):
    scenario = {
        "schema": "rgbx-scenario/1",
        "name": name,
        "description": description,
        "durationMs": duration_ms,
        "seed": 0,
        "audio": {"type": "wav", "file": f"assets/{name}.wav"},
    }
    if frames:
        scenario["imu"] = {
            "type": "keyframes",
            "frames": [
                {
                    "atMs": ms,
                    "accel": [round(v, 3) for v in accel],
                    "gyro": [round(v, 3) for v in gyro],
                }
                for ms, accel, gyro in frames
            ],
        }
    expect = {"nonBlackBeforeMs": 500, "visibleAfterBrightness": True}
    if beat_response:
        expect["beatResponse"] = True
    scenario["expect"] = expect
    return scenario


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("wav", type=Path, help="captured WAV (the .csv sidecar sits beside it)")
    parser.add_argument("--name", help="scenario name (default: the WAV's stem)")
    parser.add_argument("--description", help="scenario description — say what was ACTUALLY done")
    parser.add_argument(
        "--imu-csv", type=Path, help="override the sidecar path (default: <wav>.csv, else <wav>.imu.csv)"
    )
    parser.add_argument(
        "--hz", type=float, default=12.5, help="IMU keyframe rate after decimation (0 = keep all)"
    )
    parser.add_argument(
        "--beat-response",
        action="store_true",
        help="assert lit pixels on beat ticks exceed off-beat ticks — only for captures "
        "that actually contain music",
    )
    parser.add_argument(
        "--scenarios-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "sim" / "scenarios",
        help="where to write the scenario and its assets/",
    )
    args = parser.parse_args(argv)

    name = args.name or args.wav.stem
    # "<wav>.csv" is the current layout: one file carrying the I-rows this
    # parser wants interleaved with the D-rows beat_lab wants, because the
    # capture can only hold two FatFs handles (see the sidecar comment in
    # fw/src/sound/sound.cpp). "<wav>.imu.csv" is the older split layout and is
    # still accepted so previously-collected captures keep converting.
    # Chosen by CONTENT, not existence. On a CONFIG_APP_AUDIO_DEBUG build BOTH
    # files exist and they are not interchangeable: record_wav_tap() writes
    # "<wav>.csv" with D-rows ONLY and puts the I-rows in "<wav>.imu.csv".
    # Picking the combined file on name alone would hand this parser a
    # D-row-only file, yielding zero keyframes — and because the file exists,
    # the not-found warning below would not fire either, so the scenario would
    # come out silently audio-only.
    # Three states, not two. "Combined file has no I-rows" is NOT the same as
    # "this is the old split layout": a CONFIG_IMU=n image (or one whose IMU
    # failed to init) writes a complete <wav>.csv with every D-row and no I-row
    # at all. Collapsing those made the warning below name <wav>.imu.csv, a file
    # that cannot exist on such an image, while the real sidecar sat beside it.
    imu_csv = args.imu_csv
    combined = Path(str(args.wav) + ".csv")
    legacy = Path(str(args.wav) + ".imu.csv")
    if imu_csv is None:
        if _has_imu_rows(combined):
            imu_csv = combined          # current layout, with motion
        elif legacy.is_file():
            imu_csv = legacy            # older split layout
        else:
            imu_csv = combined          # nothing to read; warn about what IS there

    if not args.wav.is_file():
        parser.error(f"{args.wav} not found")

    duration_ms = wav_duration_ms(args.wav)

    samples = []
    if imu_csv.is_file():
        samples = parse_imu_csv(imu_csv.read_text(errors="replace"))
        samples = decimate(samples, args.hz)
        # Keyed on the parse result, not on a second existence test: the file is
        # present in this branch by definition, so an emptiness check is the only
        # thing that can distinguish "has motion" from "has none".
        if not samples:
            print(f"warning: {imu_csv} has no I, rows — scenario will have no IMU track "
                  "(CONFIG_IMU disabled, or the IMU failed to init on that capture)",
                  file=sys.stderr)
    elif args.imu_csv is not None:
        # Explicit override: name only what was actually consulted.
        print(f"warning: {imu_csv} not found — scenario will have no IMU track",
              file=sys.stderr)
    else:
        print(f"warning: no sidecar beside {args.wav} — looked for {combined.name} and "
              f"{legacy.name}; scenario will have no IMU track", file=sys.stderr)

    description = args.description or (
        f"Recorded on-device capture ({duration_ms / 1000:.1f} s, "
        f"{len(samples)} IMU keyframes)."
    )

    assets = args.scenarios_dir / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.wav, assets / f"{name}.wav")

    scenario = build_scenario(name, description, duration_ms, samples, args.beat_response)
    out = args.scenarios_dir / f"{name}.json"
    out.write_text(json.dumps(scenario, indent=2) + "\n")

    print(f"wrote {out} ({duration_ms} ms, {len(samples)} IMU keyframes)")
    print(f"wrote {assets / f'{name}.wav'}")
    print(f"run it: fw/sim/rgbx-sim run <ext> --scenario {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
