"""Pins the firmware <-> MCP-plugin contract around the capture CSV.

`handle_capture_scenario` in .serial_mcp/plugins/rgb_sunglasses.py decides
whether to hand `<wav>.csv` to beat_lab and capture_to_scenario.py by
`re.search`ing the firmware's shell output for a success line. Every failure
wording in sound.cpp is chosen so that search cannot match — a match on a
truncated, abandoned or misaligned file feeds a bad capture to every
downstream consumer.

That contract lived only in a hand-maintained comment, and it went stale on
two consecutive pushes while the gate itself was wrong in three distinct ways
across earlier review rounds — each time caught by a human reading it. So this
reads BOTH sides out of the source rather than restating them: add a failure
wording to sound.cpp, or loosen a regex in the plugin, and this fails.

Deliberately no import of the plugin module: it pulls in `mcp.types` and
`serial_mcp_server`, which are not installed in CI. Text extraction keeps the
test runnable wherever pytest is.

KNOWN GAPS — this pins less than it looks like it does (review round 8):

1. `failure_strings` anchors on literal prefixes (`"Capture CSV`,
   `"Analysis CSV`), which structurally cannot see the one regression shape
   that has actually happened here: a failure line reworded to start with
   `Audio sidecar:` simply is not harvested, so the no-false-success assertion
   never sees it. Sabotaging exactly that only tripped the fixture's own count
   check, which is luck, not coverage. Adding a prefix each time a new one
   appears is not a fix — it is the same gap re-opened at a new name, and it
   has already been hit once. It should anchor on the emitting functions
   (`audio_sidecar_close`, and `record_wav_capture`/`record_wav_tap`'s sidecar
   paths) and take every shell_warn/shell_error string in them.
2. `test_success_lines_are_still_recognised` hardcodes its three lines while
   the failure side is derived — and the success side is the half that broke
   (gating solely on "Audio sidecar: N frames" once dropped the key on the
   only build the handler can reach). It should read the shell_print formats
   out of sound.cpp the same way.

Both need a small C-source extractor (function body + adjacent-literal
joining). Until that exists, treat a green run here as necessary, not
sufficient.
"""

import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
PLUGIN = REPO / ".serial_mcp" / "plugins" / "rgb_sunglasses.py"
SOUND = REPO / "fw" / "src" / "sound" / "sound.cpp"


def _render(fmt):
    """Turn a C format string into a representative rendered line."""
    return (fmt.replace("%u", "123").replace("%d", "-4").replace("%s", "/NAND:/cap_0001.wav")
            .replace("\\n", "").replace("\\", ""))


@pytest.fixture(scope="module")
def gate_patterns():
    """The regexes the plugin uses to decide the analysis file is usable.

    Both live on `output`, and both capture a frame count — that is what makes
    them the success gate rather than the IMU one alongside them.
    """
    text = PLUGIN.read_text()
    pats = [p for p in re.findall(r're\.search\(r"([^"]+)", output\)', text) if "frames" in p]
    assert len(pats) == 2, f"expected two frame-count gates in the plugin, found {pats}"
    return [re.compile(p) for p in pats]


@pytest.fixture(scope="module")
def failure_strings():
    """Every sidecar-failure wording the firmware can print.

    Adjacent C string literals are captured as separate fragments, which is
    fine and conservative: each fragment is checked, so a wording that becomes
    matchable in any part still fails the test.

    Both prefixes are harvested. `record_wav_capture()` says "Capture CSV";
    `record_wav_tap()`'s equivalent says "Analysis CSV" — added while fixing the
    self-contradictory "ABORTED: capture incomplete - 937 of 937 frames" that
    path printed for a WAV-complete/CSV-damaged capture. That second prefix is
    exactly the blind spot KNOWN GAP 1 predicted, hit on the first new string
    written after it was documented, so it is closed here rather than noted
    again. The gap is only NARROWED, not fixed: a third prefix would still slip
    through, which is why the note below stands.
    """
    text = SOUND.read_text()
    frags = re.findall(r'"((?:Capture|Analysis) CSV[^"]*)"', text)
    assert len(frags) >= 6, f"expected at least six failure wordings, found {frags}"
    assert any(f.startswith("Analysis CSV") for f in frags), (
        "record_wav_tap()'s CSV-unusable wording is missing — if it was renamed, "
        "this harvester needs to learn the new prefix or it silently checks nothing"
    )
    return frags


def test_no_failure_wording_can_be_read_as_success(gate_patterns, failure_strings):
    for frag in failure_strings:
        line = _render(frag)
        for pat in gate_patterns:
            assert not pat.search(line), (
                f"firmware failure line {line!r} matches the plugin's success gate "
                f"{pat.pattern!r} — a bad capture would be handed downstream"
            )


@pytest.mark.parametrize("line, frames", [
    ("Audio sidecar: 625 frames", 625),
    ("Audio sidecar: 468 frames, 380 IMU samples", 468),
    ("Wrote 640000 bytes of PCM to /NAND:/cap_0001.wav (625 frames, 0 dropped, 1 io retries)", 625),
])
def test_success_lines_are_still_recognised(gate_patterns, line, frames):
    """The other half: tightening a regex until nothing matches is equally bad.

    That is not hypothetical — gating solely on "Audio sidecar: N frames" once
    dropped the key entirely on the only build the handler can reach, because
    that build's success line is the "Wrote ... (N frames, ...)" one.
    """
    hits = [p.search(line) for p in gate_patterns]
    assert any(hits), f"no gate recognises success line {line!r}"
    assert int(next(h for h in hits if h).group(1)) == frames


def test_capture_path_summary_is_not_read_as_an_analysis_file(gate_patterns):
    """`record_wav_capture()` says "blocks", `record_wav_tap()` says "frames".

    The distinction is load-bearing: the capture path announces its sidecar
    separately, so its summary line must not itself be taken as proof one was
    written — otherwise a capture whose sidecar failed to open still advertises
    `analysis_csv_path`.
    """
    line = "Wrote 640000 bytes of PCM to /NAND:/cap_0001.wav (625 blocks, 0 dropped, 0 io retries)"
    assert not any(p.search(line) for p in gate_patterns)


def test_direct_path_summary_is_not_read_as_an_analysis_file(gate_patterns):
    """`record_wav_direct()` writes no analysis CSV at all and prints no
    parenthetical, so it must fall outside both gates."""
    line = "Wrote 640000 bytes of PCM to /NAND:/cap_0001.wav"
    assert not any(p.search(line) for p in gate_patterns)
