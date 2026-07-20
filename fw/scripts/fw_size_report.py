#!/usr/bin/env python3
"""Report the firmware ROM (FLASH) + RAM footprint of the proto0 appcore image.

Two subcommands, both stdlib-only so they run on the bare CI runner:

  parse  --log <build.log> --out <size.json>
      Extract the appcore image's FLASH/RAM usage from the linker's own
      "--print-memory-usage" table that Zephyr prints at the end of each link.

  diff   --base <base.json> --head <head.json> --out <comment.md>
      Render the sticky PR-comment markdown for head-vs-base, with a warn-only
      annotation when growth crosses a threshold.

Why the linker table and not `west build -t ram_report/rom_report`?
The linker's `%age Used` is the ONLY number that governs link success, and it
counts `.noinit` buffers (e.g. the llext heap) that the footprint reports omit.
This repo treats it as ground truth — see `.claude/skills/rom-ram-budget/SKILL.md`
and the root `CLAUDE.md`. So we parse exactly what the linker printed.

A --sysbuild build prints one table per image (mcuboot, b0n, ipc_radio, fw).
The appcore application image is selected as the block with the largest FLASH
region — the appcore app slot dwarfs mcuboot / netcore / b0n, so this is a
stable, ordering-independent discriminator.
"""

import argparse
import json
import re
import sys

# Binary units, matching GNU ld's `--print-memory-usage` (KB == KiB, etc.).
_UNIT_BYTES = {"B": 1, "KB": 1024, "MB": 1024**2, "GB": 1024**3}

# A data row of the linker memory table, e.g.
#   "           FLASH:      620134 B       944 KB      64.10%"
_ROW_RE = re.compile(
    r"^\s*(?P<name>\w+):\s+"
    r"(?P<used_val>[\d.]+)\s+(?P<used_unit>B|KB|MB|GB)\s+"
    r"(?P<size_val>[\d.]+)\s+(?P<size_unit>B|KB|MB|GB)\s+"
    r"(?P<pct>[\d.]+)\s*%\s*$"
)
_HEADER_RE = re.compile(r"^\s*Memory region\s+Used Size\s+Region Size\s+%age Used")

COMMENT_MARKER = "<!-- fw-size-report -->"


def _to_bytes(value: str, unit: str) -> int:
    """Normalize a `<number> <unit>` pair from the linker table to bytes."""
    return int(round(float(value) * _UNIT_BYTES[unit]))


def parse_regions(log_text: str) -> dict:
    """Parse every memory-usage table and return the appcore image's regions.

    Returns {"flash": {"used","size","pct"}, "ram": {...}} for the block with
    the largest FLASH region. Raises ValueError if no usable table is found.
    """
    blocks: list[dict] = []
    current: dict | None = None

    for line in log_text.splitlines():
        if _HEADER_RE.search(line):
            current = {}
            blocks.append(current)
            continue
        if current is None:
            continue
        m = _ROW_RE.match(line)
        if not m:
            continue
        current[m.group("name").lower()] = {
            "used": _to_bytes(m.group("used_val"), m.group("used_unit")),
            "size": _to_bytes(m.group("size_val"), m.group("size_unit")),
            # Take the linker's printed %age verbatim rather than recomputing,
            # so our number matches the build log exactly.
            "pct": float(m.group("pct")),
        }

    candidates = [b for b in blocks if "flash" in b and "ram" in b]
    if not candidates:
        raise ValueError(
            "no linker '--print-memory-usage' table with FLASH+RAM rows found "
            "in the build log"
        )
    appcore = max(candidates, key=lambda b: b["flash"]["size"])
    return {"flash": appcore["flash"], "ram": appcore["ram"]}


def _load_json(path: str | None) -> dict | None:
    """Load a size JSON, tolerating a missing/empty/invalid baseline."""
    if not path:
        return None
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return None
    if not isinstance(data, dict) or "flash" not in data or "ram" not in data:
        return None
    return data


def _fmt_bytes(n: int) -> str:
    return f"{n:,} B"


def _fmt_cell(region: dict) -> str:
    return f"{region['used']:,} B ({region['pct']:.2f}%)"


def _fmt_delta_bytes(n: int) -> str:
    return f"{n:+,} B"


def _fmt_delta_pp(pp: float) -> str:
    return f"{pp:+.2f}"


def render_comment(base: dict | None, head: dict, warn_bytes: int, warn_pp: float):
    """Return (markdown, warn) for the sticky comment."""
    lines = [
        COMMENT_MARKER,
        "### 📦 Firmware size impact — proto0 appcore",
        "",
    ]
    warn = False

    if base is None:
        lines += [
            "| Region | This PR |",
            "|--------|---------|",
            f"| FLASH  | {_fmt_cell(head['flash'])} |",
            f"| RAM    | {_fmt_cell(head['ram'])} |",
            "",
            "_No `main` baseline available yet, so only absolute usage is shown. "
            "The baseline populates after this branch's target builds on `main`; "
            "deltas will appear on the next PR._",
        ]
        return "\n".join(lines) + "\n", warn

    lines += [
        "| Region | Base (main) | This PR | Δ bytes | Δ %pts |",
        "|--------|-------------|---------|---------|--------|",
    ]
    warn_msgs = []
    for key, label in (("flash", "FLASH"), ("ram", "RAM")):
        b, h = base[key], head[key]
        d_bytes = h["used"] - b["used"]
        d_pp = h["pct"] - b["pct"]
        lines.append(
            f"| {label}  | {_fmt_cell(b)} | {_fmt_cell(h)} | "
            f"{_fmt_delta_bytes(d_bytes)} | {_fmt_delta_pp(d_pp)} |"
        )
        if d_bytes >= warn_bytes or d_pp >= warn_pp:
            warn = True
            warn_msgs.append(
                f"**{label}** grew by {_fmt_delta_bytes(d_bytes)} "
                f"({_fmt_delta_pp(d_pp)} %pts) — over the "
                f"{warn_bytes:,} B / {warn_pp:.2f} %pts threshold."
            )

    lines.append("")
    if warn:
        lines.append("⚠️ " + "  \n⚠️ ".join(warn_msgs))
        lines.append("")
        lines.append("_This is a warning only — it does not fail the build._")
    else:
        lines.append("✅ Within the configured size threshold.")

    return "\n".join(lines) + "\n", warn


def cmd_parse(args) -> int:
    with open(args.log, encoding="utf-8") as f:
        regions = parse_regions(f.read())
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(regions, f, indent=2)
        f.write("\n")
    fl, ram = regions["flash"], regions["ram"]
    print(
        f"appcore FLASH {_fmt_bytes(fl['used'])} ({fl['pct']:.2f}%), "
        f"RAM {_fmt_bytes(ram['used'])} ({ram['pct']:.2f}%)"
    )
    return 0


def cmd_diff(args) -> int:
    head = _load_json(args.head)
    if head is None:
        # Head is our own freshly-parsed build — a missing/broken head is a
        # real failure, but keep warn-but-don't-fail: report and exit 0.
        print("::warning::fw-size-report: no valid head size JSON; skipping comment")
        return 0
    base = _load_json(args.base)
    markdown, warn = render_comment(base, head, args.warn_bytes, args.warn_pp)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(markdown)
    # Machine-readable flag for optional surfacing; never a nonzero exit.
    print(f"warn={'true' if warn else 'false'}")
    return 0


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("parse", help="extract appcore FLASH/RAM from a build log")
    p.add_argument("--log", required=True, help="path to the captured build log")
    p.add_argument("--out", required=True, help="path to write the size JSON")
    p.set_defaults(func=cmd_parse)

    d = sub.add_parser("diff", help="render the sticky PR-comment markdown")
    d.add_argument("--base", help="baseline size JSON (may be missing)")
    d.add_argument("--head", required=True, help="this PR's size JSON")
    d.add_argument("--out", required=True, help="path to write the comment markdown")
    d.add_argument("--warn-bytes", type=int, default=2048,
                   help="warn if FLASH or RAM grows by at least this many bytes")
    d.add_argument("--warn-pp", type=float, default=0.5,
                   help="warn if FLASH or RAM grows by at least this many %%-points")
    d.set_defaults(func=cmd_diff)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
