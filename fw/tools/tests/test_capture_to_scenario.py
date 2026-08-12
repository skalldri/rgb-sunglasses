"""Tests for capture_to_scenario.py (on-device capture -> sim scenario)."""

import json
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from capture_to_scenario import (  # noqa: E402
    build_scenario,
    decimate,
    main,
    parse_imu_csv,
    wav_duration_ms,
)

HEADER = "#IMU scale=1000 cols=ms,seq,ax,ay,az,gx,gy,gz units=mm/s2,mrad/s\n"


def imu_line(ms, seq, accel, gyro):
    vals = [int(v * 1000) for v in list(accel) + list(gyro)]
    return "I," + ",".join(str(v) for v in [ms, seq] + vals) + "\n"


def write_wav(path, seconds=2.0, rate=16000):
    frames = int(seconds * rate)
    data = b"\x00\x00" * frames
    fmt = struct.pack("<HHIIHH", 1, 1, rate, rate * 2, 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt
    body += b"data" + struct.pack("<I", len(data)) + data
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


def test_parses_scaled_integers_back_to_si_units():
    text = HEADER + imu_line(0, 1, (9.81, 0.0, -1.5), (0.25, -0.5, 0.125))
    (ms, accel, gyro), = parse_imu_csv(text)
    assert ms == 0
    assert accel == pytest.approx([9.81, 0.0, -1.5])
    assert gyro == pytest.approx([0.25, -0.5, 0.125])


def test_scale_comes_from_the_header_not_an_assumption():
    """The firmware states its scale; a converter that hardcoded 1000 would be
    silently wrong the day that changes."""
    text = "#IMU scale=100 cols=ms,seq,ax,ay,az,gx,gy,gz\n" + "I,0,1,981,0,0,0,0,0\n"
    (_, accel, _), = parse_imu_csv(text)
    assert accel[0] == pytest.approx(9.81)


def test_skips_foreign_lines_rather_than_rejecting_the_capture():
    """The sidecar shares a filesystem with a device that logs; one stray line
    must not throw away a recording."""
    text = HEADER + "D,1,2,3\n" + "garbage\n" + imu_line(40, 2, (1, 2, 3), (4, 5, 6)) + "I,bad\n"
    assert len(parse_imu_csv(text)) == 1


def test_orders_by_timestamp():
    text = HEADER + imu_line(80, 3, (0, 0, 0), (0, 0, 0)) + imu_line(40, 2, (0, 0, 0), (0, 0, 0))
    assert [s[0] for s in parse_imu_csv(text)] == [40, 80]


def test_decimation_thins_but_keeps_both_endpoints():
    samples = [(i * 40, [0, 0, 0], [0, 0, 0]) for i in range(26)]  # 25 Hz, 1 s
    kept = decimate(samples, 12.5)
    assert len(kept) < len(samples)
    assert kept[0][0] == 0
    assert kept[-1][0] == 1000, "the scenario must span the full capture"


def test_decimation_disabled_keeps_everything():
    samples = [(i * 40, [0, 0, 0], [0, 0, 0]) for i in range(10)]
    assert decimate(samples, 0) == samples


def test_wav_duration_read_from_the_header():
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        wav = Path(tmp) / "c.wav"
        write_wav(wav, seconds=1.5)
        assert wav_duration_ms(wav) == pytest.approx(1500, abs=2)


def test_scenario_shape_matches_what_the_loader_expects():
    frames = [(0, [9.81, 0, 0], [0, 0, 0]), (40, [9.8, 0.1, 0], [0, 0, 0.2])]
    s = build_scenario("cap", "desc", 2000, frames, beat_response=True)
    assert s["schema"] == "rgbx-scenario/1"
    # Path is relative to the scenario dir — scenarioRun resolves it that way.
    assert s["audio"] == {"type": "wav", "file": "assets/cap.wav"}
    assert s["imu"]["type"] == "keyframes"
    assert s["imu"]["frames"][1]["atMs"] == 40
    assert s["expect"]["beatResponse"] is True


def test_beat_response_is_opt_in():
    """Asserting a beat response on a capture with no music would fail forever."""
    s = build_scenario("cap", "d", 100, [], beat_response=False)
    assert "beatResponse" not in s["expect"]


def test_end_to_end_writes_scenario_and_asset(tmp_path):
    wav = tmp_path / "bobbing.wav"
    write_wav(wav, seconds=2.0)
    (tmp_path / "bobbing.wav.imu.csv").write_text(
        HEADER
        + imu_line(0, 1, (9.81, 0, 0), (0, 0, 0))
        + imu_line(1000, 2, (9.0, 3.0, 0), (0, 0, 0.5))
        + imu_line(2000, 3, (9.81, 0, 0), (0, 0, 0))
    )
    out = tmp_path / "scenarios"

    assert main([str(wav), "--scenarios-dir", str(out), "--hz", "0"]) == 0

    scenario = json.loads((out / "bobbing.json").read_text())
    assert scenario["name"] == "bobbing"
    assert scenario["durationMs"] == pytest.approx(2000, abs=2)
    assert len(scenario["imu"]["frames"]) == 3
    assert (out / "assets" / "bobbing.wav").is_file()


def test_missing_sidecar_still_produces_an_audio_only_scenario(tmp_path):
    """A capture with no IMU track is degraded, not useless."""
    wav = tmp_path / "audio_only.wav"
    write_wav(wav, seconds=1.0)
    out = tmp_path / "scenarios"

    assert main([str(wav), "--scenarios-dir", str(out)]) == 0

    scenario = json.loads((out / "audio_only.json").read_text())
    assert "imu" not in scenario
    assert scenario["audio"]["type"] == "wav"
