"""Codec for the beat-detection frame-dump format (D-lines) + WAV helpers.

Format (one line per 32 ms frame; floats are 8-hex-char IEEE-754 bit patterns,
big-endian hex of the 32-bit value, exact round-trip, produced without %f):

    #PARAMS gamma=<hex8> alpha=<hex8> floor=<hex8> refractory=<u> agc_frozen=<0|1> \
            gain=<hex2> target_low=<hex8> target_high=<hex8> rate_limit=<u>
    D,<seq>,<gain hex2>,<beatmask hex1>,<rms>,<e0..e3>,<f0..f3>,<m0..m3>,<s0..s3>[,<d0..d19>]
    #DONE frames=<u> dropped=<u>

Producers: tap_frame_format()/tap_params_format() in fw/src/sound/sound.cpp
(device "sound dump" + record_wav sidecar CSV) and the replay app in
fw/tests/sound/audio_dsp_replay/src/main.cpp. Keep all three in sync.
"""

from __future__ import annotations

import argparse
import struct
import sys
import wave
from dataclasses import dataclass, field

import numpy as np

NUM_BANDS = 4
NUM_BUCKETS = 20
FRAME_SAMPLES = 512
SAMPLE_RATE = 16000
FRAME_PERIOD_S = FRAME_SAMPLES / SAMPLE_RATE  # 32 ms


def hex_to_f32(h: str) -> float:
    """Decode an 8-hex-char IEEE-754 bit pattern (as printed by %08x) to float."""
    return struct.unpack(">f", bytes.fromhex(h))[0]


def f32_to_hex(v: float) -> str:
    return struct.pack(">f", np.float32(v)).hex()


@dataclass
class FrameDump:
    """A parsed frame dump: params header, per-frame arrays, done stats."""

    params: dict = field(default_factory=dict)
    seq: np.ndarray = None  # (N,) uint32
    gain: np.ndarray = None  # (N,) uint8, PDM gain register value
    beat: np.ndarray = None  # (N, NUM_BANDS) bool
    rms: np.ndarray = None  # (N,)
    band_energy: np.ndarray = None  # (N, NUM_BANDS)
    band_flux: np.ndarray = None  # (N, NUM_BANDS)
    band_mean: np.ndarray = None  # (N, NUM_BANDS)
    band_sigma: np.ndarray = None  # (N, NUM_BANDS)
    buckets: np.ndarray = None  # (N, NUM_BUCKETS) or None
    frames_reported: int = None  # from #DONE, None if absent
    dropped: int = None

    @property
    def times(self) -> np.ndarray:
        """Frame timestamps in seconds on the WAV timeline (capture-index based).

        Deliberately NOT derived from the device seq counter: the WAV contains
        only the frames that were actually captured, so when frames were dropped
        the seq-derived wall-clock timeline diverges from the audio — and every
        consumer of these times (librosa references computed from the WAV,
        spectrograms, host replay output) is WAV-relative. Use seq_gaps() /
        contiguous_runs() to reason about drops; a gapped capture's timeline is
        compressed relative to wall clock by 32 ms per dropped frame.
        """
        return np.arange(len(self.seq)) * FRAME_PERIOD_S

    def beat_times(self, band: int | None = None) -> np.ndarray:
        """Times of frames where a beat fired (any band, or one band)."""
        mask = self.beat.any(axis=1) if band is None else self.beat[:, band]
        return self.times[mask]

    def seq_gaps(self) -> list[tuple[int, int]]:
        """(from_seq, to_seq) pairs where frames were dropped."""
        d = np.diff(self.seq.astype(np.int64))
        idx = np.nonzero(d != 1)[0]
        return [(int(self.seq[i]), int(self.seq[i + 1])) for i in idx]

    def contiguous_runs(self) -> list[slice]:
        """Index slices of contiguous-seq segments (for gap-aware analysis)."""
        d = np.diff(self.seq.astype(np.int64))
        breaks = np.nonzero(d != 1)[0] + 1
        edges = [0, *breaks.tolist(), len(self.seq)]
        return [slice(a, b) for a, b in zip(edges[:-1], edges[1:])]


def _parse_params(line: str) -> dict:
    out = {}
    for tok in line.split()[1:]:
        key, _, val = tok.partition("=")
        if key in ("gamma", "alpha", "floor", "target_low", "target_high", "gate"):
            out[key] = hex_to_f32(val)
        elif key == "gain":
            out[key] = int(val, 16)
        else:
            out[key] = int(val)
    return out


def parse_dump(path_or_lines) -> FrameDump:
    """Parse a frame dump from a file path or an iterable of lines.

    Tolerates interleaved noise (shell echo, log lines): only lines starting
    with 'D,' / '#PARAMS' / '#DONE' are consumed.
    """
    if isinstance(path_or_lines, (str, bytes)):
        with open(path_or_lines, "r", errors="replace") as f:
            lines = f.readlines()
    else:
        lines = list(path_or_lines)

    dump = FrameDump()
    rows = []
    for line in lines:
        line = line.strip()
        if line.startswith("#PARAMS"):
            dump.params = _parse_params(line)
        elif line.startswith("#DONE"):
            for tok in line.split()[1:]:
                key, _, val = tok.partition("=")
                if key == "frames":
                    dump.frames_reported = int(val)
                elif key == "dropped":
                    dump.dropped = int(val)
        elif line.startswith("D,"):
            rows.append(line.split(","))

    if not rows:
        raise ValueError("no D-lines found in input")

    n_fields = len(rows[0])
    expected_base = 4 + 1 + 4 * NUM_BANDS  # prefix(4) + rms + 4 band arrays
    has_buckets = n_fields == expected_base + NUM_BUCKETS
    if not has_buckets and n_fields != expected_base:
        raise ValueError(f"unexpected D-line field count {n_fields}")

    n = len(rows)
    dump.seq = np.empty(n, dtype=np.uint32)
    dump.gain = np.empty(n, dtype=np.uint8)
    dump.beat = np.zeros((n, NUM_BANDS), dtype=bool)
    floats = np.empty((n, n_fields - 4), dtype=np.float32)
    for i, p in enumerate(rows):
        if len(p) != n_fields:
            raise ValueError(f"inconsistent D-line field count at row {i}")
        dump.seq[i] = int(p[1])
        dump.gain[i] = int(p[2], 16)
        mask = int(p[3], 16)
        for b in range(NUM_BANDS):
            dump.beat[i, b] = bool(mask & (1 << b))
        floats[i] = [hex_to_f32(h) for h in p[4:]]

    dump.rms = floats[:, 0]
    dump.band_energy = floats[:, 1 : 1 + NUM_BANDS]
    dump.band_flux = floats[:, 1 + NUM_BANDS : 1 + 2 * NUM_BANDS]
    dump.band_mean = floats[:, 1 + 2 * NUM_BANDS : 1 + 3 * NUM_BANDS]
    dump.band_sigma = floats[:, 1 + 3 * NUM_BANDS : 1 + 4 * NUM_BANDS]
    if has_buckets:
        dump.buckets = floats[:, 1 + 4 * NUM_BANDS :]
    return dump


def format_frame(seq, gain, beat_mask, rms, energy, flux, mean, sigma, buckets=None) -> str:
    """Inverse of one D-line parse — used by tests to round-trip the codec."""
    parts = [f"D,{seq},{gain:02x},{beat_mask:x}", f32_to_hex(rms)]
    for arr in (energy, flux, mean, sigma):
        parts.extend(f32_to_hex(v) for v in arr)
    if buckets is not None:
        parts.extend(f32_to_hex(v) for v in buckets)
    return ",".join(parts)


def read_wav(path: str) -> np.ndarray:
    """Read a 16 kHz mono 16-bit WAV to an int16 array.

    Python's wave module skips unknown RIFF chunks, so the JUNK padding chunk
    record_wav inserts for sector alignment is handled transparently.
    """
    with wave.open(path, "rb") as w:
        if w.getnchannels() != 1 or w.getframerate() != SAMPLE_RATE or w.getsampwidth() != 2:
            raise ValueError(
                f"{path}: expected 16 kHz mono 16-bit PCM, got "
                f"{w.getframerate()} Hz / {w.getnchannels()} ch / {8 * w.getsampwidth()} bit"
            )
        return np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")


def write_wav(path: str, samples: np.ndarray) -> None:
    """Write an int16 array as a 16 kHz mono WAV (canonical 44-byte header)."""
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(np.asarray(samples, dtype="<i2").tobytes())


def synth_click_track(duration_s: float, bpm: float, click_freq: float = 100.0,
                      click_len_s: float = 0.05, amplitude: float = 0.9) -> tuple[np.ndarray, np.ndarray]:
    """Synthesize a click track (sine bursts on silence) for tests.

    Returns (samples int16, click_times_seconds).
    """
    n = int(duration_s * SAMPLE_RATE)
    samples = np.zeros(n, dtype=np.float64)
    period = 60.0 / bpm
    click_n = int(click_len_s * SAMPLE_RATE)
    t = np.arange(click_n) / SAMPLE_RATE
    # Instant attack + exponential decay: onset detectors (and the firmware's
    # spectral flux) key on sharp attacks; a symmetric fade-in envelope would
    # smear the onset across frames.
    burst = np.sin(2 * np.pi * click_freq * t) * np.exp(-t / 0.01)
    times = []
    pos = 0.1  # lead-in silence so detector history can settle
    while pos + click_len_s < duration_s:
        i = int(pos * SAMPLE_RATE)
        samples[i : i + click_n] += burst
        times.append(pos)
        pos += period
    samples = np.clip(samples * amplitude, -1.0, 1.0)
    return (samples * 32767).astype(np.int16), np.array(times)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Inspect a beat-detection frame dump")
    ap.add_argument("dump", help="frame dump file (D-line format)")
    args = ap.parse_args(argv)

    d = parse_dump(args.dump)
    print(f"frames: {len(d.seq)}  seq {d.seq[0]}..{d.seq[-1]}  gaps: {len(d.seq_gaps())}")
    if d.params:
        print(f"params: {d.params}")
    if d.frames_reported is not None:
        print(f"#DONE: frames={d.frames_reported} dropped={d.dropped}")
    for b in range(NUM_BANDS):
        print(f"band {b}: {int(d.beat[:, b].sum())} beats, "
              f"mean energy {d.band_energy[:, b].mean():.6g}, "
              f"mean flux {d.band_flux[:, b].mean():.6g}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
