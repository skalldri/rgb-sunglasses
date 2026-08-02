"""Compare a device capture against a host replay of the same WAV.

This is the acceptance gate for the replay harness: the host build of
audio_dsp.cpp must reproduce the device's per-frame analysis to within float
noise. Differences beyond tolerance mean the harness has drifted from the
firmware and its tuning results cannot be trusted.

Known, expected difference sources (reported, not failed):
  - Warm-up: the device's flux history/prev-energy state is warm when the tap
    arms; the host starts cold. The first HISTORY_LEN (32) frames are excluded
    from beat-mask scoring by default (--include-warmup to override).
  - Seq gaps: dropped frames leave the WAV missing those samples, so the frame
    AFTER each gap sees a different prev-frame — excluded like warm-up frames.
  - Float noise: Cortex-M33 VFMA contraction vs host SSE. Borderline threshold
    comparisons can flip a beat flag; mismatches are reported with the
    threshold margin so real divergence is distinguishable from noise.
"""

from __future__ import annotations

import argparse
import sys

import numpy as np

from . import frames

WARMUP_FRAMES = 32  # HISTORY_LEN in audio_dsp.cpp


def align(device: frames.FrameDump, host: frames.FrameDump):
    """Pair frames positionally, not by seq offset.

    Device CSV row k and host replay frame k both describe the k-th 512-sample
    block of the same WAV: record_wav writes exactly the tapped frames, in
    order, and the replay walks that WAV front to back. The seq counter is the
    WRONG join key — it keeps counting across dropped frames, so a seq-offset
    map would slide every post-gap device frame onto a later host frame (the
    dropped frames' audio simply isn't in the WAV). Seq is used only for gap
    detection/exclusion.
    """
    n = min(len(device.seq), len(host.seq))
    idx = np.arange(n)
    return idx, idx


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--device", required=True, help="device sidecar CSV / dump capture")
    ap.add_argument("--host", required=True, help="host replay output on the same WAV")
    ap.add_argument("--rtol", type=float, default=1e-4,
                    help="relative tolerance on field differences (default 1e-4)")
    ap.add_argument("--atol", type=float, default=1e-5,
                    help="absolute tolerance floor — flux is a difference of two "
                         "log values, so near-zero values carry large relative "
                         "float-cancellation noise (default 1e-5)")
    ap.add_argument("--include-warmup", action="store_true",
                    help="score warm-up and post-gap frames too")
    args = ap.parse_args(argv)

    dev = frames.parse_dump(args.device)
    host = frames.parse_dump(args.host)

    # A device-vs-host comparison is only meaningful if both ran the same
    # detector parameters — persisted device values routinely differ from the
    # replay's compiled-in defaults (hardware-observed: a device alpha of 1.5 vs
    # the default 3.5 produced 175 spurious beat-mask "mismatches").
    params_mismatch = False
    for key in ("gamma", "alpha", "floor", "refractory", "gain", "target_low", "target_high",
                "rate_limit", "attack", "release", "gate"):
        dv, hv = dev.params.get(key), host.params.get(key)
        if dv is not None and hv is not None and abs(float(dv) - float(hv)) > 1e-6 * max(
                abs(float(dv)), 1.0):
            print(f"WARNING: #PARAMS mismatch '{key}': device={dv} host={hv}")
            params_mismatch = True
    if params_mismatch:
        print(f"         beat decisions are NOT comparable - re-run the replay with "
              f"--params-from {args.device}")

    di, hi = align(dev, host)
    if len(di) == 0:
        print("FAIL: no aligned frames", file=sys.stderr)
        return 1
    print(f"aligned frames: {len(di)} (device {len(dev.seq)}, host {len(host.seq)}, "
          f"{len(dev.seq_gaps())} device gaps)")

    # Frames whose detector state is expected to differ: warm-up + first frame
    # after each seq gap (the WAV is missing the dropped samples).
    expected_diff = np.zeros(len(di), dtype=bool)
    if not args.include_warmup:
        expected_diff[:WARMUP_FRAMES] = True
        gap_after = {b for _, b in dev.seq_gaps()}
        for k, i in enumerate(di):
            if int(dev.seq[i]) in gap_after:
                lo = k
                hi_k = min(len(di), k + WARMUP_FRAMES)
                expected_diff[lo:hi_k] = True

    scored = ~expected_diff

    # Beat-mask agreement.
    dev_masks = dev.beat[di]
    host_masks = host.beat[hi]
    mask_eq = (dev_masks == host_masks).all(axis=1)
    n_scored = int(scored.sum())
    n_match = int((mask_eq & scored).sum())
    print(f"beat masks identical: {n_match}/{n_scored} scored frames "
          f"({100.0 * n_match / max(n_scored, 1):.2f}%)"
          + (f" [{int((~mask_eq & ~scored).sum())} more differ in excluded frames]"
             if (~scored).any() else ""))

    # Field-level differences on energy/flux (the raw DSP outputs), judged with
    # np.allclose-style |a-b| <= atol + rtol*|b| semantics.
    worst = (0.0, None)
    for name, dfield, hfield in [("energy", dev.band_energy, host.band_energy),
                                 ("flux", dev.band_flux, host.band_flux),
                                 ("rms", dev.rms[:, None], host.rms[:, None])]:
        a = dfield[di][scored]
        b = hfield[hi][scored]
        excess = np.abs(a - b) - (args.atol + args.rtol * np.maximum(np.abs(a), np.abs(b)))
        m = float(excess.max()) if excess.size else 0.0
        print(f"  {name}: max abs diff {float(np.abs(a - b).max()) if a.size else 0:.3g}, "
              f"tolerance excess {m:+.3g}")
        if m > worst[0]:
            worst = (m, name)

    # Explain each scored beat-mask mismatch with its threshold margin.
    bad = np.nonzero(~mask_eq & scored)[0]
    for k in bad[:10]:
        i, j = di[k], hi[k]
        for b in range(frames.NUM_BANDS):
            if dev.beat[i, b] != host.beat[j, b]:
                margin_d = dev.band_flux[i, b] - (dev.band_mean[i, b]
                                                  + dev.params.get("alpha", 3.5) * dev.band_sigma[i, b])
                print(f"  mismatch seq={int(dev.seq[i])} band={b}: device={bool(dev.beat[i, b])} "
                      f"host={bool(host.beat[j, b])} device threshold margin={margin_d:+.4g}")
    if len(bad) > 10:
        print(f"  ... and {len(bad) - 10} more mismatches")

    ok = worst[0] <= 0.0 and n_match == n_scored
    print("PASS" if ok else
          f"MARGINAL (tolerance excess {worst[0]:.3g} in {worst[1]}, "
          f"{n_scored - n_match} beat-mask flips) — inspect margins above")
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
