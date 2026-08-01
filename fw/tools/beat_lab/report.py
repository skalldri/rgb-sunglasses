"""Render a diagnostic report PNG/SVG for a beat-detection capture or replay.

Stacked, time-aligned small multiples (one y-axis per measure — never dual):

    spectrogram (single-hue sequential ramp, dB)
    per-band spectral flux + adaptive threshold + detected beats   (x4)
    block RMS
    PDM gain register (dB)

Reference beats (from --ref / --ref-librosa) appear as neutral dashed vlines
across the flux rows, so hits/misses/false-positives are visible at a glance.

Colors are the project dataviz reference palette (categorical slots 1-4 for the
bands, fixed order, identity by hue + direct label). Grid and axes stay
recessive; text wears text colors, never series colors.
"""

from __future__ import annotations

import argparse
import sys

import numpy as np

from . import evaluate, frames

# Dataviz reference palette, light mode (see the dataviz skill's palette.md).
SURFACE = "#ffffff"
TEXT_PRIMARY = "#1a1a19"
TEXT_SECONDARY = "#5f5e56"
GRID = "#e3e2da"
REFERENCE = "#8a897f"  # neutral — reference marks are not a series
BAND_COLORS = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]  # slots 1-4
BAND_LABELS = ["bass 31-200 Hz", "low-mid 219-781 Hz", "mid 813-1969 Hz", "high 2-6 kHz"]


def build_figure(dump: frames.FrameDump, wav: np.ndarray | None, ref_times, alpha: float):
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    n_rows = 1 + frames.NUM_BANDS + 2
    fig, axes = plt.subplots(
        n_rows, 1, sharex=True, figsize=(14, 1.6 * n_rows),
        gridspec_kw={"height_ratios": [2.2] + [1] * frames.NUM_BANDS + [1, 0.8]},
    )
    fig.patch.set_facecolor(SURFACE)
    t = dump.times
    duration = float(t[-1]) + frames.FRAME_PERIOD_S

    for ax in axes:
        ax.set_facecolor(SURFACE)
        for spine in ax.spines.values():
            spine.set_color(GRID)
        ax.tick_params(colors=TEXT_SECONDARY, labelsize=8)
        ax.grid(True, color=GRID, linewidth=0.6, alpha=0.6)

    # 1. Spectrogram — sequential single-hue ramp (magnitude job).
    ax = axes[0]
    if wav is not None:
        f_spec, t_spec, sxx = _spectrogram(wav)
        db = 10 * np.log10(sxx + 1e-12)
        ax.pcolormesh(t_spec, f_spec, db, cmap="Blues", shading="auto",
                      vmin=np.percentile(db, 20), vmax=db.max())
        ax.set_ylim(0, 6000)
        ax.set_ylabel("Hz", color=TEXT_SECONDARY, fontsize=8)
    ax.set_title("beat_lab report", color=TEXT_PRIMARY, fontsize=11, loc="left")

    # 2. Per-band flux vs threshold, detected + reference beats.
    for b in range(frames.NUM_BANDS):
        ax = axes[1 + b]
        color = BAND_COLORS[b]
        threshold = dump.band_mean[:, b] + alpha * dump.band_sigma[:, b]
        for rt in ref_times:
            ax.axvline(rt, color=REFERENCE, linewidth=0.8, linestyle="--", alpha=0.6, zorder=1)
        ax.plot(t, dump.band_flux[:, b], color=color, linewidth=1.2, zorder=3)
        ax.plot(t, threshold, color=color, linewidth=1.0, linestyle=":", alpha=0.7, zorder=2)
        bt = dump.times[dump.beat[:, b]]
        bf = dump.band_flux[dump.beat[:, b], b]
        ax.plot(bt, bf, "o", color=color, markersize=5, markeredgecolor=SURFACE,
                markeredgewidth=1.0, zorder=4)
        # Direct label (identity), text token color for the sublabel.
        ax.text(0.005, 0.92, BAND_LABELS[b], transform=ax.transAxes, fontsize=8.5,
                color=color, va="top", fontweight="bold")
        ax.text(0.005, 0.62, f"{int(dump.beat[:, b].sum())} beats  (flux / ⋯ threshold)",
                transform=ax.transAxes, fontsize=7.5, color=TEXT_SECONDARY, va="top")
        ax.set_ylabel("flux", color=TEXT_SECONDARY, fontsize=8)

    # 3. RMS.
    ax = axes[1 + frames.NUM_BANDS]
    ax.plot(t, dump.rms, color=BAND_COLORS[0], linewidth=1.2)
    ax.set_ylabel("RMS", color=TEXT_SECONDARY, fontsize=8)

    # 4. Gain register as dB (step line — it changes in 0.5 dB quanta).
    ax = axes[2 + frames.NUM_BANDS]
    gain_db = dump.gain.astype(float) * 0.5 - 20.0
    ax.step(t, gain_db, where="post", color=TEXT_PRIMARY, linewidth=1.2)
    ax.set_ylabel("gain dB", color=TEXT_SECONDARY, fontsize=8)
    ax.set_xlabel("time (s)", color=TEXT_SECONDARY, fontsize=9)

    # Mark seq gaps (dropped frames) across every panel.
    for a, bseq in dump.seq_gaps():
        x = (a + 1 - int(dump.seq[0])) * frames.FRAME_PERIOD_S
        for ax in axes[1:]:
            ax.axvline(x, color=TEXT_SECONDARY, linewidth=0.6, alpha=0.4)

    axes[0].set_xlim(0, duration)
    fig.align_ylabels(axes)
    fig.tight_layout()
    return fig


def _spectrogram(samples: np.ndarray):
    """512-pt Hann spectrogram matching the firmware's analysis window."""
    try:
        from scipy.signal import spectrogram as sp_spec

        f, t, sxx = sp_spec(samples.astype(np.float32) / 32768.0, fs=frames.SAMPLE_RATE,
                            window="hann", nperseg=512, noverlap=256)
        return f, t, sxx
    except ImportError:
        n = 512
        hop = 256
        win = np.hanning(n)
        x = samples.astype(np.float32) / 32768.0
        n_frames = max((len(x) - n) // hop + 1, 1)
        sxx = np.empty((n // 2 + 1, n_frames), dtype=np.float32)
        for i in range(n_frames):
            seg = x[i * hop : i * hop + n] * win
            sxx[:, i] = np.abs(np.fft.rfft(seg)) ** 2
        f = np.fft.rfftfreq(n, 1.0 / frames.SAMPLE_RATE)
        t = (np.arange(n_frames) * hop + n / 2) / frames.SAMPLE_RATE
        return f, t, sxx


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--frames", required=True, help="frame dump (device CSV or replay output)")
    ap.add_argument("--wav", help="matching WAV (adds the spectrogram panel)")
    ap.add_argument("--ref", help="reference annotation file (seconds per line)")
    ap.add_argument("--ref-librosa", choices=["beats", "onsets"])
    ap.add_argument("--out", default="beat_report.png", help="output image path (.png/.svg)")
    args = ap.parse_args(argv)

    dump = frames.parse_dump(args.frames)
    wav = frames.read_wav(args.wav) if args.wav else None
    ref_times = []
    if args.ref or args.ref_librosa:
        ref_times = evaluate.load_reference(args.ref, args.ref_librosa, args.wav)
    alpha = dump.params.get("alpha", 3.5)

    fig = build_figure(dump, wav, ref_times, alpha)
    fig.savefig(args.out, dpi=130, facecolor=SURFACE)
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
