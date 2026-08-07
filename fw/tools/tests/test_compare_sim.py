"""Tests for compare_sim.py (WASM simulator DSP vs native_sim replay).

The gate-logic tests are pure numpy and always run. The full end-to-end
parity test needs BOTH pipelines' artifacts (a west-built native_sim replay
binary AND the wasm toolchain), so it only runs when the two dump paths are
supplied via environment variables — CI's firmware workflow provides them:

    COMPARE_SIM_HOST=<host dump> COMPARE_SIM_SIM=<sim dump> \
        pytest tools/tests/test_compare_sim.py -v

Run from fw/: pytest tools/tests/test_compare_sim.py -v
"""

import os
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.beat_lab import compare_sim, frames  # noqa: E402


def test_ulp_distance_basics():
    a = np.array([1.0, -1.0, 0.0], dtype=np.float32)
    assert list(compare_sim.ulp_distance(a, a)) == [0, 0, 0]
    # One ULP up from 1.0f.
    b = np.array([np.nextafter(np.float32(1.0), np.float32(2.0)), -1.0, 0.0], dtype=np.float32)
    assert list(compare_sim.ulp_distance(a, b)) == [1, 0, 0]
    # Sign-crossing distance goes through zero (monotonic bit-space mapping).
    c = np.array([np.float32(-0.0), 0.0], dtype=np.float32)
    d = np.array([np.float32(0.0), np.float32(1e-45)], dtype=np.float32)
    dist = compare_sim.ulp_distance(c, d)
    assert dist[0] == 0 or dist[0] == 1  # -0.0 vs 0.0 maps within one step
    assert dist[1] == 1  # smallest subnormal is 1 ULP from zero


def _dump_from_arrays(energy, beat_mask_per_frame):
    """Builds D-line text from (N,4) energies; flux/mean/sigma mirror energy,
    buckets are a tiled copy."""
    lines = []
    for i, e in enumerate(energy):
        buckets = np.tile(e, 5)
        lines.append(
            frames.format_frame(i, 0x28, beat_mask_per_frame[i], 0.5, e, e, e, e, buckets)
        )
    lines.append(f"#DONE frames={len(energy)} dropped=0")
    return lines


def test_compare_accepts_identical_dumps(tmp_path):
    rng = np.random.default_rng(7)
    energy = rng.random((30, 4)).astype(np.float32)
    masks = [1 if i % 10 == 0 else 0 for i in range(30)]
    a = tmp_path / "a.txt"
    b = tmp_path / "b.txt"
    a.write_text("\n".join(_dump_from_arrays(energy, masks)) + "\n")
    b.write_text("\n".join(_dump_from_arrays(energy, masks)) + "\n")
    assert compare_sim.compare(str(a), str(b), compare_sim.MAX_ULP,
                               compare_sim.MAX_BEAT_MISMATCH_FRAC) == 0


def test_compare_scale_relative_gate_passes_noise_floor(tmp_path):
    # A big relative error on a value ~1e9x below the array scale must pass
    # (FFT cancellation noise), while the same relative error at full scale
    # must fail.
    energy = np.ones((20, 4), dtype=np.float32)
    energy[:, 0] = 100.0  # array scale
    modified = energy.copy()
    modified[5, 1] = np.float32(1e-7 * 1.001)  # near-zero, 0.1% relative off
    energy[5, 1] = np.float32(1e-7)
    masks = [0] * 20
    a = tmp_path / "a.txt"
    b = tmp_path / "b.txt"
    a.write_text("\n".join(_dump_from_arrays(energy, masks)) + "\n")
    b.write_text("\n".join(_dump_from_arrays(modified, masks)) + "\n")
    assert compare_sim.compare(str(a), str(b), compare_sim.MAX_ULP,
                               compare_sim.MAX_BEAT_MISMATCH_FRAC) == 0

    # Same absolute-scale trick at full scale: 1% off at the array max.
    worse = energy.copy()
    worse[7, 0] = np.float32(101.0)
    b.write_text("\n".join(_dump_from_arrays(worse, masks)) + "\n")
    assert compare_sim.compare(str(a), str(b), compare_sim.MAX_ULP,
                               compare_sim.MAX_BEAT_MISMATCH_FRAC) == 1


def test_compare_beat_mismatch_budget(tmp_path):
    energy = np.ones((200, 4), dtype=np.float32)
    masks_a = [0] * 200
    masks_b = [0] * 200
    masks_b[3] = 1  # 1 frame of 200 = 0.5% <= 1% budget
    a = tmp_path / "a.txt"
    b = tmp_path / "b.txt"
    a.write_text("\n".join(_dump_from_arrays(energy, masks_a)) + "\n")
    b.write_text("\n".join(_dump_from_arrays(energy, masks_b)) + "\n")
    assert compare_sim.compare(str(a), str(b), compare_sim.MAX_ULP,
                               compare_sim.MAX_BEAT_MISMATCH_FRAC) == 0
    for i in range(3, 30):  # 27 frames = 13.5% > 1%
        masks_b[i] = 1
    b.write_text("\n".join(_dump_from_arrays(energy, masks_b)) + "\n")
    assert compare_sim.compare(str(a), str(b), compare_sim.MAX_ULP,
                               compare_sim.MAX_BEAT_MISMATCH_FRAC) == 1


@pytest.mark.skipif(
    not (os.environ.get("COMPARE_SIM_HOST") and os.environ.get("COMPARE_SIM_SIM")),
    reason="end-to-end parity dumps not supplied (COMPARE_SIM_HOST/COMPARE_SIM_SIM)",
)
def test_full_parity_from_artifacts():
    assert compare_sim.compare(
        os.environ["COMPARE_SIM_HOST"],
        os.environ["COMPARE_SIM_SIM"],
        compare_sim.MAX_ULP,
        compare_sim.MAX_BEAT_MISMATCH_FRAC,
    ) == 0
