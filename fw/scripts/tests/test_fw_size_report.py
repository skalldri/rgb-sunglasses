"""Unit tests for fw/scripts/fw_size_report.py."""

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent))
from fw_size_report import (  # noqa: E402
    COMMENT_MARKER,
    parse_regions,
    render_comment,
    _to_bytes,
    cmd_diff,
)

# A trimmed but faithful capture of a --sysbuild build: four
# "--print-memory-usage" tables, one per image. The fw appcore image has by
# far the largest FLASH region, which is how parse_regions identifies it.
SYSBUILD_LOG = """\
-- west build: building application (mcuboot)
[600/600] Linking C executable zephyr/zephyr.elf
Memory region         Used Size  Region Size  %age Used
           FLASH:       48232 B        64 KB      73.60%
             RAM:        9600 B       128 KB       7.32%
        IDT_LIST:          0 GB        32 KB       0.00%

-- west build: building application (b0n)
[120/120] Linking C executable zephyr/zephyr.elf
Memory region         Used Size  Region Size  %age Used
           FLASH:        6144 B         8 KB      75.00%
             RAM:        2048 B        16 KB      12.50%

-- west build: building application (ipc_radio)
[900/900] Linking C executable zephyr/zephyr.elf
Memory region         Used Size  Region Size  %age Used
           FLASH:      196608 B       256 KB      75.00%
             RAM:       32768 B        64 KB      50.00%

-- west build: building application (fw)
[2400/2400] Linking C executable zephyr/zephyr.elf
Memory region         Used Size  Region Size  %age Used
           FLASH:      620134 B       944 KB      64.10%
             RAM:      294912 B       448 KB      64.29%
        IDT_LIST:          0 GB        32 KB       0.00%
"""


class TestUnitNormalization:
    def test_bytes(self):
        assert _to_bytes("620134", "B") == 620134

    def test_kb_is_kib(self):
        assert _to_bytes("944", "KB") == 944 * 1024

    def test_mb(self):
        assert _to_bytes("1", "MB") == 1024 * 1024

    def test_fractional_kb(self):
        assert _to_bytes("1.5", "KB") == 1536


class TestParseRegions:
    def test_selects_appcore_by_largest_flash_region(self):
        regions = parse_regions(SYSBUILD_LOG)
        # 944 KB FLASH region == the fw appcore image, not mcuboot/b0n/netcore.
        assert regions["flash"]["size"] == 944 * 1024
        assert regions["flash"]["used"] == 620134
        assert regions["flash"]["pct"] == 64.10
        assert regions["ram"]["used"] == 294912
        assert regions["ram"]["size"] == 448 * 1024
        assert regions["ram"]["pct"] == 64.29

    def test_ignores_non_flash_ram_rows(self):
        regions = parse_regions(SYSBUILD_LOG)
        assert set(regions) == {"flash", "ram"}

    def test_raises_when_no_table(self):
        with pytest.raises(ValueError):
            parse_regions("just some build output\nwith no memory table\n")

    def test_single_image_log(self):
        single = "\n".join(SYSBUILD_LOG.splitlines()[-6:])
        regions = parse_regions(single)
        assert regions["flash"]["used"] == 620134


BASE = {
    "flash": {"used": 620134, "size": 944 * 1024, "pct": 64.10},
    "ram": {"used": 294912, "size": 448 * 1024, "pct": 64.29},
}


class TestRenderComment:
    def test_small_growth_no_warn(self):
        head = {
            "flash": {"used": 620540, "size": 944 * 1024, "pct": 64.15},
            "ram": {"used": 294912, "size": 448 * 1024, "pct": 64.29},
        }
        md, warn = render_comment(BASE, head, warn_bytes=2048, warn_pp=0.5)
        assert warn is False
        assert COMMENT_MARKER in md
        assert "+406 B" in md
        assert "✅" in md
        assert "⚠️" not in md

    def test_large_flash_growth_warns(self):
        head = {
            "flash": {"used": 620134 + 5000, "size": 944 * 1024, "pct": 64.63},
            "ram": {"used": 294912, "size": 448 * 1024, "pct": 64.29},
        }
        md, warn = render_comment(BASE, head, warn_bytes=2048, warn_pp=0.5)
        assert warn is True
        assert "⚠️" in md
        assert "+5,000 B" in md
        assert "FLASH" in md

    def test_shrink_never_warns(self):
        head = {
            "flash": {"used": 620134 - 10000, "size": 944 * 1024, "pct": 63.04},
            "ram": {"used": 294912 - 4096, "size": 448 * 1024, "pct": 63.40},
        }
        md, warn = render_comment(BASE, head, warn_bytes=2048, warn_pp=0.5)
        assert warn is False
        assert "-10,000 B" in md

    def test_pp_threshold_warns_even_if_bytes_small(self):
        head = {
            "flash": {"used": 620134 + 100, "size": 944 * 1024, "pct": 65.10},
            "ram": {"used": 294912, "size": 448 * 1024, "pct": 64.29},
        }
        md, warn = render_comment(BASE, head, warn_bytes=100000, warn_pp=0.5)
        assert warn is True

    def test_missing_baseline_shows_absolute_only(self):
        md, warn = render_comment(None, BASE, warn_bytes=2048, warn_pp=0.5)
        assert warn is False
        assert "No `main` baseline" in md
        assert "620,134 B" in md
        assert "Δ" not in md


class TestCmdDiff:
    def _write(self, path, obj):
        path.write_text(json.dumps(obj))
        return str(path)

    def test_regression_exits_zero(self, tmp_path):
        head = {
            "flash": {"used": 620134 + 50000, "size": 944 * 1024, "pct": 69.4},
            "ram": {"used": 294912, "size": 448 * 1024, "pct": 64.29},
        }
        base_p = self._write(tmp_path / "base.json", BASE)
        head_p = self._write(tmp_path / "head.json", head)
        out_p = tmp_path / "comment.md"
        args = _Args(base=base_p, head=head_p, out=str(out_p),
                     warn_bytes=2048, warn_pp=0.5)
        # warn-but-don't-fail: even a big regression returns 0.
        assert cmd_diff(args) == 0
        assert "⚠️" in out_p.read_text()

    def test_missing_baseline_file_is_tolerated(self, tmp_path):
        head_p = self._write(tmp_path / "head.json", BASE)
        out_p = tmp_path / "comment.md"
        args = _Args(base=str(tmp_path / "does-not-exist.json"), head=head_p,
                     out=str(out_p), warn_bytes=2048, warn_pp=0.5)
        assert cmd_diff(args) == 0
        assert "No `main` baseline" in out_p.read_text()

    def test_missing_head_exits_zero_without_comment(self, tmp_path):
        out_p = tmp_path / "comment.md"
        args = _Args(base=None, head=str(tmp_path / "nope.json"),
                     out=str(out_p), warn_bytes=2048, warn_pp=0.5)
        assert cmd_diff(args) == 0
        assert not out_p.exists()


class _Args:
    """Minimal argparse.Namespace stand-in for cmd_diff."""

    def __init__(self, **kw):
        self.__dict__.update(kw)
