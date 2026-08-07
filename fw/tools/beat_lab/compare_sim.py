"""Compare the WASM simulator's DSP output against the native_sim replay.

The extension simulator (fw/sim/) compiles the same audio_dsp.cpp + CMSIS-DSP
translation units to WebAssembly; this script proves the two builds compute
the same thing for the same WAV, within float-platform tolerance:

    python3 fw/tools/beat_lab/replay.py --wav clip.wav --buckets --out host.txt
    fw/sim/rgbx-sim dsp-replay --wav clip.wav --out sim.txt
    python3 fw/tools/beat_lab/compare_sim.py host.txt sim.txt

Gates (tuned for x86 vs wasm float differences — both are strict IEEE-754
for +-*/ but libm differs (glibc vs wasi-libc: cosf in the Hann window,
log1pf in the flux path) and those last-bit differences compound through
the FFT and history statistics). Measured on an 8 s click track with
byte-identical PCM input: max scale-relative error 3.7e-7, beats 0/250
mismatched. Per element a value passes if EITHER:
  - ULP distance <= 64 (loud values agree tightly), OR
  - |host - sim| <= 2e-5 * max|host array| (near-zero values suffer
    catastrophic cancellation in the FFT, where a 1-ULP window difference
    becomes a large RELATIVE error on a physically-meaningless noise-floor
    number; 2e-5 is ~50x margin over the measured worst case)
Beat masks: <= 1% of frames may disagree (threshold-crossing flips on
borderline frames are expected; systematic disagreement is not).

The rms and gain columns are NOT compared: the sim emitter has no AGC loop
(gain fixed at 0) and documents its rms as informational.

Exit code 0 on pass, 1 on gate failure or input mismatch.
"""

from __future__ import annotations

import argparse
import sys

import numpy as np

try:
    from . import frames  # package import (pytest, python3 -m), like compare.py
except ImportError:  # direct script invocation: python3 fw/tools/beat_lab/compare_sim.py
    import frames  # type: ignore[no-redef]

parse_dump = frames.parse_dump

MAX_ULP = 64
SCALE_RTOL = 2e-5
MAX_BEAT_MISMATCH_FRAC = 0.01


def ulp_distance(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    """Element-wise ULP distance between two float32 arrays.

    Uses the standard bit-space trick: map the sign-magnitude int32 view to
    a monotonic (two's-complement-ordered) space, then diff. Handles
    mixed-sign near-zero pairs correctly (distance through zero).
    """
    ai = np.frombuffer(np.asarray(a, dtype="<f4").tobytes(), dtype="<i4").astype(np.int64)
    bi = np.frombuffer(np.asarray(b, dtype="<f4").tobytes(), dtype="<i4").astype(np.int64)
    ai = np.where(ai < 0, np.int64(-(2**31)) - ai, ai)
    bi = np.where(bi < 0, np.int64(-(2**31)) - bi, bi)
    return np.abs(ai - bi)


def compare(host_path: str, sim_path: str, max_ulp: int, max_beat_frac: float,
            scale_rtol: float = SCALE_RTOL) -> int:
    host = parse_dump(host_path)
    sim = parse_dump(sim_path)

    n = min(len(host.seq), len(sim.seq))
    if n == 0:
        print("no overlapping frames", file=sys.stderr)
        return 1
    if abs(len(host.seq) - len(sim.seq)) > 1:
        # One trailing partial frame of slack (the two front-ends round the
        # tail differently); more than that means different inputs.
        print(f"frame count mismatch: host={len(host.seq)} sim={len(sim.seq)}", file=sys.stderr)
        return 1

    failures = 0
    for name in ("band_energy", "band_flux", "band_mean", "band_sigma", "buckets"):
        h = getattr(host, name)
        s = getattr(sim, name)
        if h is None or s is None:
            if name == "buckets":
                print("buckets: skipped (absent in one dump — pass --buckets to replay.py)")
                continue
            print(f"{name}: missing", file=sys.stderr)
            failures += 1
            continue
        hv = h[:n].ravel()
        sv = s[:n].ravel()
        d = ulp_distance(hv, sv)
        scale = float(np.abs(hv).max()) or 1.0
        absd = np.abs(hv.astype(np.float64) - sv.astype(np.float64))
        # Pass per element on EITHER gate (see module docstring).
        bad = (d > max_ulp) & (absd > scale_rtol * scale)
        rel = float(absd.max()) / scale
        if bad.any():
            failures += 1
            worst_idx = int(np.argmax(np.where(bad, absd, 0)))
            frame, elem = divmod(worst_idx, h.shape[1])
            print(
                f"{name}: FAIL {int(bad.sum())} element(s) outside both gates; worst at "
                f"frame {frame} elem {elem} (host={hv[worst_idx]:.9g} sim={sv[worst_idx]:.9g}, "
                f"scale-relative {absd[worst_idx] / scale:.3g} > {scale_rtol:g})",
            )
        else:
            print(
                f"{name}: OK max ULP {int(d.max())}, max scale-relative "
                f"{rel:.3g} (gates: ULP<={max_ulp} or rel<={scale_rtol:g})",
            )

    mismatch = (host.beat[:n] != sim.beat[:n]).any(axis=1)
    frac = float(mismatch.mean())
    beat_status = "OK" if frac <= max_beat_frac else "FAIL"
    print(
        f"beats: {beat_status} {int(mismatch.sum())}/{n} frames disagree "
        f"({100 * frac:.2f}%, gate {100 * max_beat_frac:.0f}%)",
    )
    if frac > max_beat_frac:
        failures += 1

    return 1 if failures else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("host_dump", help="D-line dump from the native_sim replay harness")
    ap.add_argument("sim_dump", help="D-line dump from `rgbx-sim dsp-replay`")
    ap.add_argument("--max-ulp", type=int, default=MAX_ULP)
    ap.add_argument("--max-beat-mismatch", type=float, default=MAX_BEAT_MISMATCH_FRAC)
    ap.add_argument("--scale-rtol", type=float, default=SCALE_RTOL)
    args = ap.parse_args(argv)
    return compare(args.host_dump, args.sim_dump, args.max_ulp, args.max_beat_mismatch,
                   args.scale_rtol)


if __name__ == "__main__":
    sys.exit(main())
