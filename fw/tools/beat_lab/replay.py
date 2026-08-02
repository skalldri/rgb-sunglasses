"""Run the native_sim replay harness over a WAV, optionally sweeping parameters.

Wraps `west build` (incremental) of fw/tests/sound/audio_dsp_replay plus the
env-var-driven zephyr.exe invocation, filtering the Zephyr boot banner so the
output is a clean frame dump (see frames.py for the format).

Examples (run from the repo root):

    python3 fw/tools/beat_lab/replay.py --wav rec.wav --out host.txt
    python3 fw/tools/beat_lab/replay.py --wav rec.wav --alpha 2.5 --floor 0.02
    python3 fw/tools/beat_lab/replay.py --wav rec.wav \
        --sweep "alpha=2.0:5.0:0.5,floor=0.005:0.05:0.005" --ref-librosa beats

Sweep mode re-runs the prebuilt binary per combination (params are env-driven,
no recompile) and prints an F-measure table via evaluate.py.
"""

from __future__ import annotations

import argparse
import itertools
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
APP_DIR = REPO_ROOT / "fw" / "tests" / "sound" / "audio_dsp_replay"
BUILD_DIR = REPO_ROOT / "fw" / "build_beat_replay"
EXE = BUILD_DIR / "audio_dsp_replay" / "zephyr" / "zephyr.exe"


def build(force: bool = False) -> None:
    """Incremental west build of the replay app (seconds when unchanged)."""
    if EXE.exists() and not force:
        # Rebuild anyway — west is a fast no-op when nothing changed, and this
        # guarantees code edits to audio_dsp.cpp are always picked up.
        pass
    cmd = [
        "west", "build",
        "--build-dir", str(BUILD_DIR),
        str(APP_DIR),
        "--board", "native_sim/native/64",
    ]
    res = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True)
    if res.returncode != 0:
        sys.stderr.write(res.stdout[-4000:] + res.stderr[-4000:])
        raise RuntimeError("west build of the replay app failed")


def run_replay(wav: str, *, gamma=None, alpha=None, floor=None, refractory=None,
               agc: str = "off", gain: int | None = None, buckets: bool = False,
               target_low=None, target_high=None, rate_limit=None, gate=None,
               attack=None, release=None, sf_delta=None, threshold_mode=None) -> list[str]:
    """Run one replay; returns the dump lines (banner filtered)."""
    env = dict(os.environ)
    env["BEAT_WAV"] = str(Path(wav).resolve())
    env["BEAT_AGC"] = agc
    for name, val in [("BEAT_GAMMA", gamma), ("BEAT_ALPHA", alpha), ("BEAT_FLOOR", floor),
                      ("BEAT_REFRACTORY", refractory), ("BEAT_GAIN", gain),
                      ("BEAT_TARGET_LOW", target_low), ("BEAT_TARGET_HIGH", target_high),
                      ("BEAT_RATE_LIMIT", rate_limit), ("BEAT_GATE", gate),
                      ("BEAT_ATTACK", attack), ("BEAT_RELEASE", release),
                      ("BEAT_SF_DELTA", sf_delta), ("BEAT_THRESHOLD_MODE", threshold_mode)]:
        if val is not None:
            env[name] = str(val)
    if buckets:
        env["BEAT_BUCKETS"] = "1"
    res = subprocess.run([str(EXE)], env=env, capture_output=True, text=True)
    if res.returncode != 0:
        sys.stderr.write(res.stderr[-2000:])
        raise RuntimeError(f"replay exited {res.returncode}")
    return [l for l in res.stdout.splitlines()
            if l.startswith(("D,", "#PARAMS", "#DONE"))]


def parse_sweep(spec: str) -> dict[str, list[float]]:
    """Parse 'alpha=2.0:5.0:0.5,floor=0.005:0.05:0.005' into value lists."""
    out = {}
    for part in spec.split(","):
        name, _, rng = part.partition("=")
        name = name.strip()
        if name not in ("gamma", "alpha", "floor", "refractory", "sf_delta", "threshold_mode"):
            raise ValueError(f"unknown sweep parameter '{name}'")
        lo, hi, step = (float(x) for x in rng.split(":"))
        vals, v = [], lo
        while v <= hi + 1e-9:
            vals.append(round(v, 10))
            v += step
        out[name] = vals
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--wav", required=True, help="input 16 kHz mono 16-bit WAV")
    ap.add_argument("--out", help="write the frame dump here (default stdout)")
    ap.add_argument("--gamma", type=float)
    ap.add_argument("--alpha", type=float)
    ap.add_argument("--floor", type=float)
    ap.add_argument("--refractory", type=int)
    ap.add_argument("--sf-delta", type=float,
                    help="mode-1 offset above the running median (BEAT_SF_DELTA)")
    ap.add_argument("--threshold-mode", type=int, choices=(0, 1),
                    help="0 = mean+alpha*sigma (default), 1 = median+sf_delta")
    ap.add_argument("--agc", choices=["off", "sim", "sim_legacy", "sim_reset"], default="off",
                    help="off = fixed gain; sim = the real AgcController (Phase 2 policy); "
                         "sim_legacy = pre-Phase-2 symmetric window + compensation; "
                         "sim_reset = pre-Phase-1 full-reset-per-step (A/B chain)")
    ap.add_argument("--target-low", type=float, help="AGC sim targetLow (BEAT_TARGET_LOW)")
    ap.add_argument("--target-high", type=float, help="AGC sim targetHigh (BEAT_TARGET_HIGH)")
    ap.add_argument("--gate", type=float, help="AGC sim noise-gate RMS (BEAT_GATE)")
    ap.add_argument("--attack", type=int, help="AGC sim attack frames (BEAT_ATTACK)")
    ap.add_argument("--release", type=int, help="AGC sim release frames (BEAT_RELEASE)")
    ap.add_argument("--rate-limit", type=int, help="AGC sim min-gap frames (BEAT_RATE_LIMIT)")
    ap.add_argument("--gain", type=lambda s: int(s, 0),
                    help="recording's PDM gain register value (default 0x28)")
    ap.add_argument("--buckets", action="store_true", help="include display buckets")
    ap.add_argument("--no-build", action="store_true", help="skip the west build step")
    ap.add_argument("--params-from", metavar="DEVICE_CSV",
                    help="take gamma/alpha/floor/refractory and the recording gain from a "
                         "device capture's #PARAMS line, so the replay runs with the exact "
                         "parameters the device used (explicit flags still override). Use "
                         "this for every device-vs-host comparison — persisted device "
                         "values routinely differ from compiled-in defaults")
    ap.add_argument("--sweep", help="grid sweep, e.g. 'alpha=2:5:0.5,floor=0.005:0.05:0.005'")
    ap.add_argument("--ref", help="(sweep) reference annotation file, seconds per line")
    ap.add_argument("--ref-librosa", choices=["beats", "onsets"],
                    help="(sweep) compute reference from the WAV with librosa")
    ap.add_argument("--band", type=int, default=None,
                    help="(sweep) score this band only (default: any-band union)")
    args = ap.parse_args(argv)

    if not args.no_build:
        build()

    if args.params_from:
        try:
            from . import frames
        except ImportError:
            sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
            from tools.beat_lab import frames
        p = frames.parse_dump(args.params_from).params
        # Copy EVERY parameter the #PARAMS line carries — omitting any of them
        # makes the replay trajectory silently diverge from the device's.
        for attr, key in [("gamma", "gamma"), ("alpha", "alpha"), ("floor", "floor"),
                          ("refractory", "refractory"), ("gain", "gain"),
                          ("target_low", "target_low"), ("target_high", "target_high"),
                          ("rate_limit", "rate_limit"), ("attack", "attack"),
                          ("release", "release"), ("gate", "gate"),
                          ("sf_delta", "sf_delta"), ("threshold_mode", "mode")]:
            if getattr(args, attr) is None and key in p:
                setattr(args, attr, p[key])
        gain_str = "-" if args.gain is None else f"{args.gain:#04x}"
        print(f"# params from {args.params_from}: gamma={args.gamma} alpha={args.alpha} "
              f"floor={args.floor} refractory={args.refractory} gain={gain_str} "
              f"targets=[{args.target_low},{args.target_high}] rate={args.rate_limit} "
              f"attack={args.attack} release={args.release} gate={args.gate} "
              f"sf_delta={args.sf_delta} mode={args.threshold_mode}",
              file=sys.stderr)

    fixed = dict(gamma=args.gamma, alpha=args.alpha, floor=args.floor,
                 refractory=args.refractory, agc=args.agc, gain=args.gain,
                 target_low=args.target_low, target_high=args.target_high, gate=args.gate,
                 attack=args.attack, release=args.release, rate_limit=args.rate_limit,
                 sf_delta=args.sf_delta, threshold_mode=args.threshold_mode)

    if not args.sweep:
        lines = run_replay(args.wav, buckets=args.buckets, **fixed)
        text = "\n".join(lines) + "\n"
        if args.out:
            Path(args.out).write_text(text)
            print(f"wrote {len(lines)} lines to {args.out}", file=sys.stderr)
        else:
            sys.stdout.write(text)
        return 0

    # Sweep mode: score every combination against the reference.
    # Local imports keep non-sweep use librosa-free; the fallback makes direct
    # script invocation (python3 fw/tools/beat_lab/replay.py) work too, where
    # relative imports have no parent package.
    try:
        from . import evaluate, frames
    except ImportError:
        sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
        from tools.beat_lab import evaluate, frames

    ref_times = evaluate.load_reference(args.ref, args.ref_librosa, args.wav)
    grid = parse_sweep(args.sweep)
    names = list(grid)
    print("\t".join(names + ["precision", "recall", "f_measure", "beats"]))
    best = None
    for combo in itertools.product(*(grid[n] for n in names)):
        params = dict(fixed)
        params.update(dict(zip(names, combo)))
        dump = frames.parse_dump(run_replay(args.wav, **params))
        det = dump.beat_times(args.band)
        p, r, f = evaluate.score(det, ref_times)
        print("\t".join([f"{v:g}" for v in combo] + [f"{p:.3f}", f"{r:.3f}", f"{f:.3f}",
                                                     str(len(det))]))
        if best is None or f > best[0]:
            best = (f, dict(zip(names, combo)))
    print(f"# best: f={best[0]:.3f} at {best[1]}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
