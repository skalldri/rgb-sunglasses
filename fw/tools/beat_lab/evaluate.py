"""Score detected beats against a reference with MIREX-standard F-measure.

Reference sources:
  --ref FILE           plain text, one onset/beat time (seconds) per line —
                       Audacity label-track export format works (first column)
  --ref-librosa MODE   compute the reference from the WAV with librosa:
                       'beats' (librosa.beat.beat_track) or 'onsets'
                       (librosa.onset.onset_detect)

Scoring uses mir_eval.onset.f_measure when available (the MIREX evaluator,
default 50 ms tolerance window) and falls back to an equivalent greedy
bipartite matcher so the pure-numpy tests can run without mir_eval installed.
Detected beat times are quantized to the 32 ms frame grid — keep --window at
or above 50 ms unless you account for that.
"""

from __future__ import annotations

import argparse
import json
import sys

import numpy as np

from . import frames

DEFAULT_WINDOW_S = 0.05


def load_reference(ref_path: str | None, ref_librosa: str | None, wav_path: str | None):
    """Load reference times from a file or compute them with librosa."""
    if (ref_path is None) == (ref_librosa is None):
        raise ValueError("provide exactly one of --ref / --ref-librosa")
    if ref_path is not None:
        times = []
        with open(ref_path) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    times.append(float(line.split()[0]))
        return np.array(sorted(times))

    import librosa  # deferred: heavy import, numba JIT on first use

    y, sr = librosa.load(wav_path, sr=None, mono=True)
    if ref_librosa == "beats":
        _, beat_frames = librosa.beat.beat_track(y=y, sr=sr)
        times = librosa.frames_to_time(beat_frames, sr=sr)
    else:
        times = librosa.onset.onset_detect(y=y, sr=sr, units="time")
    # Enforce a minimum inter-onset interval: librosa's peak picker can emit
    # doubled onsets one hop apart on a single percussive event, which would
    # unfairly penalize precision. 60 ms ≈ the shortest musically distinct gap.
    deduped = []
    for t in times:
        if not deduped or t - deduped[-1] >= 0.06:
            deduped.append(float(t))
    return np.array(deduped)


def score(detected: np.ndarray, reference: np.ndarray,
          window: float = DEFAULT_WINDOW_S) -> tuple[float, float, float]:
    """(precision, recall, f_measure) with a ±window matching tolerance."""
    try:
        import mir_eval

        f, p, r = mir_eval.onset.f_measure(np.asarray(reference, dtype=float),
                                           np.asarray(detected, dtype=float),
                                           window=window)
        return p, r, f
    except ImportError:
        return _score_greedy(detected, reference, window)


def _score_greedy(detected, reference, window: float) -> tuple[float, float, float]:
    # Greedy one-to-one matcher, the documented degraded path when mir_eval is
    # not installed (kept separate so tests exercise it even when mir_eval is).
    # Scans EVERY reference candidate inside the window (not a fixed lookahead):
    # dense references can put several events within one window of a detection,
    # and stopping early under-counts matches.
    det = sorted(detected)
    ref = sorted(reference)
    if not det or not ref:
        return (0.0, 0.0, 0.0) if (det or ref) else (1.0, 1.0, 1.0)
    matched = 0
    j = 0
    used = [False] * len(ref)
    for t in det:
        while j < len(ref) and ref[j] < t - window:
            j += 1
        k = j
        while k < len(ref) and ref[k] <= t + window:
            if not used[k]:
                used[k] = True
                matched += 1
                break
            k += 1
    p = matched / len(det)
    r = matched / len(ref)
    f = 2 * p * r / (p + r) if p + r > 0 else 0.0
    return p, r, f


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--frames", required=True, help="frame dump (device CSV or replay output)")
    ap.add_argument("--wav", help="WAV path (required for --ref-librosa)")
    ap.add_argument("--ref", help="reference annotation file (seconds per line)")
    ap.add_argument("--ref-librosa", choices=["beats", "onsets"])
    ap.add_argument("--window", type=float, default=DEFAULT_WINDOW_S,
                    help=f"matching tolerance in seconds (default {DEFAULT_WINDOW_S})")
    ap.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = ap.parse_args(argv)

    dump = frames.parse_dump(args.frames)
    ref = load_reference(args.ref, args.ref_librosa, args.wav)

    results = {}
    for band in range(frames.NUM_BANDS):
        p, r, f = score(dump.beat_times(band), ref, args.window)
        results[f"band{band}"] = {"precision": p, "recall": r, "f_measure": f,
                                  "beats": int(dump.beat[:, band].sum())}
    p, r, f = score(dump.beat_times(None), ref, args.window)
    results["union"] = {"precision": p, "recall": r, "f_measure": f,
                        "beats": int(dump.beat.any(axis=1).sum())}
    results["reference_count"] = len(ref)
    results["window_s"] = args.window
    results["gaps"] = len(dump.seq_gaps())

    if args.json:
        json.dump(results, sys.stdout, indent=2)
        print()
    else:
        print(f"reference: {len(ref)} events, window ±{args.window * 1000:.0f} ms, "
              f"{len(dump.seq)} frames ({results['gaps']} gaps)")
        for name in [f"band{b}" for b in range(frames.NUM_BANDS)] + ["union"]:
            s = results[name]
            print(f"  {name:6s}  P={s['precision']:.3f}  R={s['recall']:.3f}  "
                  f"F={s['f_measure']:.3f}  ({s['beats']} beats)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
