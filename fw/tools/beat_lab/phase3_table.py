"""Phase 3 acceptance table: mode 0 (mean+alpha*sigma) vs mode 1 (median+delta).

Produces, per corpus clip, the F-vs-parameter curve for both threshold modes,
plus the two summary statistics the decision rule needs:

  * best F per mode per clip   — "is mode 1 at least as good everywhere?"
  * the F spread of a SINGLE shared parameter value across clips, and the
    curve's flatness — "does one setting work across venues?", which is the
    entire justification for preferring the median shape.

Run from the repo root:
    python3 fw/tools/beat_lab/phase3_table.py            # full corpus
    python3 fw/tools/beat_lab/phase3_table.py --band 0
"""

from __future__ import annotations

import argparse
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.beat_lab import evaluate, frames, replay  # noqa: E402

CORPUS = Path("fw/testdata/beat-corpus")
ALPHAS = [round(0.1 + 0.1 * i, 2) for i in range(12)]
DELTAS = [round(0.02 + 0.02 * i, 2) for i in range(20)]


def curve(wav, csv, band, mode, values):
    """F-measure for each parameter value, at fixed device-captured params."""
    p = frames.parse_dump(str(csv)).params
    ref = evaluate.load_reference(None, "beats", str(wav))
    out = []
    for v in values:
        kwargs = dict(
            gamma=p.get("gamma"), floor=p.get("floor"), refractory=p.get("refractory"),
            gain=p.get("gain"), agc="off", threshold_mode=mode,
        )
        if mode == 0:
            kwargs["alpha"] = v
        else:
            kwargs["sf_delta"] = v
        dump = frames.parse_dump(replay.run_replay(str(wav), **kwargs))
        _, _, f = evaluate.score(dump.beat_times(band), ref)
        out.append(f)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--band", type=int, default=0)
    ap.add_argument("--clips", nargs="*", default=["base60", "loud30", "newbase"])
    args = ap.parse_args(argv)

    replay.build()
    results = {}
    for clip in args.clips:
        wav, csv = CORPUS / f"{clip}.wav", CORPUS / f"{clip}.wav.csv"
        results[clip] = {
            0: curve(wav, csv, args.band, 0, ALPHAS),
            1: curve(wav, csv, args.band, 1, DELTAS),
        }
        print(f"# {clip} done", file=sys.stderr)

    print(f"\n=== Band {args.band} F-measure, +/-50 ms vs librosa beats ===\n")
    for mode, values, label in ((0, ALPHAS, "alpha"), (1, DELTAS, "sf_delta")):
        print(f"-- mode {mode} ({'mean+alpha*sigma' if mode == 0 else 'median+sf_delta'}) --")
        print(f"{label:>9} " + " ".join(f"{c:>9}" for c in args.clips) + "     spread")
        for i, v in enumerate(values):
            row = [results[c][mode][i] for c in args.clips]
            print(f"{v:>9} " + " ".join(f"{f:>9.3f}" for f in row)
                  + f"   {max(row) - min(row):.3f}")
        print()

    print("=== summary ===")
    for clip in args.clips:
        b0 = max(results[clip][0])
        b1 = max(results[clip][1])
        a0 = ALPHAS[results[clip][0].index(b0)]
        d1 = DELTAS[results[clip][1].index(b1)]
        verdict = "mode1" if b1 >= b0 else "mode0"
        print(f"{clip:>9}: mode0 best {b0:.3f} @alpha={a0}   "
              f"mode1 best {b1:.3f} @delta={d1}   -> {verdict}")

    # Robustness: how much does a clip lose by running ONE shared setting
    # instead of its own per-clip optimum? This is the claim that justifies
    # mode 1 — a single delta that travels, versus an alpha that must be
    # retuned per venue.
    print("\n=== single shared setting (max total regret vs per-clip best) ===")
    for mode, values, label in ((0, ALPHAS, "alpha"), (1, DELTAS, "sf_delta")):
        best_shared, best_regret = None, None
        for i, v in enumerate(values):
            regret = max(max(results[c][mode]) - results[c][mode][i] for c in args.clips)
            if best_regret is None or regret < best_regret:
                best_shared, best_regret = v, regret
        worst = min(results[c][mode][values.index(best_shared)] for c in args.clips)
        print(f"mode {mode}: best shared {label}={best_shared} -> worst-clip F {worst:.3f}, "
              f"max regret {best_regret:.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
