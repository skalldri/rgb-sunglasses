"""Tests for the beat_lab tooling (fw/tools/beat_lab).

Pure-numpy core tests always run; librosa/mir_eval-dependent tests are skipped
when those packages are absent so a wheel gap degrades rather than breaks CI.
Run from fw/: pytest tools/tests/test_beat_lab.py -v
"""

import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.beat_lab import compare, evaluate, frames  # noqa: E402


def _make_dump_lines(n=40, buckets=False, seq0=100, beat_at=(10, 25)):
    rng = np.random.default_rng(42)
    lines = [
        "#PARAMS gamma=447a0000 alpha=40600000 floor=3ba3d70a refractory=5 "
        "agc_frozen=1 gain=28 target_low=3ba3d70a target_high=3c03126f rate_limit=10 "
        "attack=3 release=15 gate=3a83126f"
    ]
    for i in range(n):
        mask = 0x1 if i in beat_at else 0
        energy = rng.random(4).astype(np.float32)
        flux = rng.random(4).astype(np.float32) * 0.1
        mean = rng.random(4).astype(np.float32) * 0.05
        sigma = rng.random(4).astype(np.float32) * 0.01
        bk = rng.random(20).astype(np.float32) if buckets else None
        lines.append(frames.format_frame(seq0 + i, 0x28, mask, 0.01 * i, energy, flux,
                                         mean, sigma, bk))
    lines.append(f"#DONE frames={n} dropped=0")
    return lines


class TestCodec:
    def test_roundtrip(self):
        lines = _make_dump_lines()
        d = frames.parse_dump(lines)
        assert len(d.seq) == 40
        assert d.seq[0] == 100 and d.seq[-1] == 139
        assert d.params["gamma"] == pytest.approx(1000.0)
        assert d.params["alpha"] == pytest.approx(3.5)
        assert d.params["floor"] == pytest.approx(0.005, rel=1e-6)
        assert d.params["gain"] == 0x28
        assert d.params["attack"] == 3 and d.params["release"] == 15
        assert d.params["gate"] == pytest.approx(0.001, rel=1e-5)
        assert d.frames_reported == 40 and d.dropped == 0
        assert d.beat[10, 0] and d.beat[25, 0] and not d.beat[11, 0]
        assert d.buckets is None

    def test_roundtrip_buckets(self):
        d = frames.parse_dump(_make_dump_lines(buckets=True))
        assert d.buckets.shape == (40, frames.NUM_BUCKETS)

    def test_float_bits_exact(self):
        for v in (0.0, 1.0, 3.5, 0.005, 1e-30, -2.75):
            assert frames.hex_to_f32(frames.f32_to_hex(v)) == np.float32(v)

    def test_tolerates_noise_lines(self):
        lines = ["uart:~$ sound dump 3", *_make_dump_lines(3), "[00:00:01] <inf> junk"]
        assert len(frames.parse_dump(lines).seq) == 3

    def test_seq_gaps(self):
        lines = _make_dump_lines(10)
        # Remove one D-line in the middle to fake a dropped frame.
        del lines[5]
        d = frames.parse_dump(lines)
        assert d.seq_gaps() == [(103, 105)]
        runs = d.contiguous_runs()
        assert len(runs) == 2

    def test_times_are_wav_relative_across_gaps(self):
        # Dropped frames are absent from the WAV, so times must advance one
        # frame period per CAPTURED frame (capture index), not per seq step —
        # a seq-based timeline would leave a 64 ms hole the audio doesn't have.
        lines = _make_dump_lines(10)
        del lines[5]
        d = frames.parse_dump(lines)
        assert len(d.times) == 9
        assert np.allclose(np.diff(d.times), frames.FRAME_PERIOD_S)

    def test_wav_roundtrip(self, tmp_path):
        samples, _ = frames.synth_click_track(2.0, 120)
        p = str(tmp_path / "t.wav")
        frames.write_wav(p, samples)
        back = frames.read_wav(p)
        assert np.array_equal(back, samples)


class TestEvaluate:
    def test_perfect_score(self):
        ref = np.array([1.0, 2.0, 3.0])
        p, r, f = evaluate.score(ref.copy(), ref)
        assert (p, r, f) == (1.0, 1.0, 1.0)

    def test_offset_within_window(self):
        ref = np.array([1.0, 2.0, 3.0])
        det = ref + 0.03  # inside the 50 ms window
        _, _, f = evaluate.score(det, ref)
        assert f == 1.0

    def test_miss_and_false_positive(self):
        ref = np.array([1.0, 2.0, 3.0, 4.0])
        det = np.array([1.0, 2.0, 9.0])  # 2 hits, 1 FP, 2 misses
        p, r, _ = evaluate.score(det, ref)
        assert p == pytest.approx(2 / 3)
        assert r == pytest.approx(2 / 4)

    def test_empty_detections(self):
        p, r, f = evaluate.score(np.array([]), np.array([1.0]))
        assert (p, r, f) == (0.0, 0.0, 0.0)

    def test_dense_reference_all_matched(self):
        # Several reference events inside one window: the greedy matcher must
        # scan every in-window candidate, not a fixed 2-candidate lookahead
        # (regression for the PR #275 review finding). Tests the fallback
        # matcher directly — score() would route to mir_eval when installed.
        ref = np.array([1.00, 1.02, 1.04])
        det = np.array([1.00, 1.01, 1.03])
        p, r, f = evaluate._score_greedy(det, ref, window=0.05)
        assert (p, r, f) == (1.0, 1.0, 1.0)
        p, r, f = evaluate.score(det, ref, window=0.05)
        assert (p, r, f) == (1.0, 1.0, 1.0)

    def test_ref_file(self, tmp_path):
        p = tmp_path / "ref.txt"
        p.write_text("0.5\tlabel\n1.25\n# comment\n")
        times = evaluate.load_reference(str(p), None, None)
        assert list(times) == [0.5, 1.25]


class TestCompare:
    def test_identical_pass(self, capsys):
        lines = _make_dump_lines(n=40)
        d = frames.parse_dump(lines)
        h = frames.parse_dump(lines)
        di, hi = compare.align(d, h)
        assert len(di) == 40

    def test_align_offset(self):
        dev = frames.parse_dump(_make_dump_lines(n=10, seq0=500))
        host = frames.parse_dump(_make_dump_lines(n=10, seq0=0))
        di, hi = compare.align(dev, host)
        assert len(di) == 10
        assert list(di) == list(hi)

    def test_cli_pass(self, tmp_path, capsys):
        p = tmp_path / "d.txt"
        p.write_text("\n".join(_make_dump_lines(n=40)) + "\n")
        rc = compare.main(["--device", str(p), "--host", str(p)])
        assert rc == 0
        assert "PASS" in capsys.readouterr().out

    def test_warns_on_params_mismatch(self, tmp_path, capsys):
        # Device ran alpha=1.5 (0x3fc00000) while the host replay defaulted to
        # 3.5 (0x40600000): decisions aren't comparable and compare must say so
        # (regression for a real footgun hit during hardware verification).
        lines = _make_dump_lines(n=40)
        host_lines = [lines[0].replace("alpha=40600000", "alpha=3fc00000")] + lines[1:]
        pd = tmp_path / "d.txt"
        pd.write_text("\n".join(lines) + "\n")
        ph = tmp_path / "h.txt"
        ph.write_text("\n".join(host_lines) + "\n")
        compare.main(["--device", str(pd), "--host", str(ph)])
        out = capsys.readouterr().out
        assert "PARAMS mismatch 'alpha'" in out
        assert "--params-from" in out


class TestReplayCli:
    def test_params_from_without_gain_does_not_crash(self, tmp_path, monkeypatch, capsys):
        # A #PARAMS line without a gain field (older captures, hand-trimmed
        # dumps) must not crash the summary print (PR #279 review: NoneType
        # format TypeError).
        from tools.beat_lab import replay as replay_mod

        lines = _make_dump_lines(n=3)
        lines[0] = lines[0].replace(" gain=28", "")
        src = tmp_path / "old.csv"
        src.write_text("\n".join(lines) + "\n")
        wav = tmp_path / "in.wav"
        samples, _ = frames.synth_click_track(1.0, 120)
        frames.write_wav(str(wav), samples)

        monkeypatch.setattr(replay_mod, "build", lambda force=False: None)
        monkeypatch.setattr(replay_mod, "run_replay",
                            lambda *a, **k: _make_dump_lines(n=3))
        out = tmp_path / "out.txt"
        rc = replay_mod.main(["--wav", str(wav), "--params-from", str(src),
                              "--out", str(out)])
        assert rc == 0
        assert "gain=-" in capsys.readouterr().err

    def test_params_from_copies_agc_targets(self, tmp_path, monkeypatch):
        # --params-from must reproduce the device's FULL AGC configuration —
        # omitting targets made offline sims follow a trajectory no board runs
        # (PR #279 review).
        from tools.beat_lab import replay as replay_mod

        src = tmp_path / "dev.csv"
        src.write_text("\n".join(_make_dump_lines(n=3)) + "\n")
        wav = tmp_path / "in.wav"
        samples, _ = frames.synth_click_track(1.0, 120)
        frames.write_wav(str(wav), samples)

        captured = {}

        def fake_run(*a, **k):
            captured.update(k)
            return _make_dump_lines(n=3)

        monkeypatch.setattr(replay_mod, "build", lambda force=False: None)
        monkeypatch.setattr(replay_mod, "run_replay", fake_run)
        replay_mod.main(["--wav", str(wav), "--params-from", str(src),
                        "--out", str(tmp_path / "o.txt")])
        assert captured["target_low"] == pytest.approx(0.005, rel=1e-5)
        assert captured["target_high"] == pytest.approx(0.008, rel=1e-5)
        assert captured["rate_limit"] == 10
        assert captured["attack"] == 3
        assert captured["release"] == 15
        assert captured["gate"] == pytest.approx(0.001, rel=1e-5)


@pytest.mark.filterwarnings("ignore")
class TestWithLibrosa:
    def test_librosa_reference_on_clicks(self, tmp_path):
        pytest.importorskip("librosa")
        samples, click_times = frames.synth_click_track(6.0, 120)
        wav = str(tmp_path / "clicks.wav")
        frames.write_wav(wav, samples)
        times = evaluate.load_reference(None, "onsets", wav)
        # librosa should find most of the synthetic clicks near their true times
        p, r, f = evaluate.score(times, click_times, window=0.07)
        assert f > 0.8

    def test_mir_eval_matches_fallback(self):
        pytest.importorskip("mir_eval")
        rng = np.random.default_rng(7)
        ref = np.sort(rng.uniform(0, 30, 40))
        det = np.sort(np.concatenate([ref[:30] + rng.normal(0, 0.02, 30),
                                      rng.uniform(0, 30, 8)]))
        import mir_eval

        f_m, p_m, r_m = mir_eval.onset.f_measure(ref, det, window=0.05)
        # sanity only: both implementations agree loosely on a random case
        # (_score_greedy directly — score() routes to mir_eval when installed)
        p_f, r_f, f_f = evaluate._score_greedy(det, ref, 0.05)
        assert abs(f_m - f_f) < 0.1
