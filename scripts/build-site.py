#!/usr/bin/env python3
"""Render selected repo Markdown docs into the static Pages site under `site/`.

Single source of truth: the Markdown files listed in DOCS below are authoritative;
this script renders each into a styled HTML page that matches the hand-written site
(site/index.html, site/privacy/index.html). It runs in .github/workflows/pages.yml
before the Pages upload, and can be run locally to preview:

    pip install markdown
    python3 scripts/build-site.py
    # then open site/recovery/index.html in a browser

The generated <out>/index.html files are git-ignored — regenerate them, never edit
them by hand (edits would be lost and drift from the Markdown source).
"""
import os
import sys

try:
    import markdown
    import pymdownx  # noqa: F401  (provides the superfences extension used below)
except ImportError:
    sys.exit(
        "error: required packages missing — pip install markdown pymdown-extensions"
    )

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SITE_DIR = os.path.join(REPO_ROOT, "site")

# Nav links shared across generated pages (mirrors site/index.html's nav).
# (href, label, external?)
NAV_LINKS = [
    ("https://github.com/skalldri/rgb-sunglasses", "GitHub", True),
    ("https://github.com/skalldri/rgb-sunglasses/releases", "Releases", True),
    ("/recovery", "Recovery", False),
    ("/privacy", "Privacy", False),
]

# Docs to publish. Each entry:
#   src   — repo-relative Markdown path (the single source of truth)
#   out   — output dir under site/ (page served at /<out>)
#   title — page title (also used in <title>)
#   desc  — meta description
DOCS = [
    {
        "src": "fw/docs/flashing-without-jlink.md",
        "out": "recovery",
        "title": "Firmware Recovery",
        "desc": "Update or recover RGB Sunglasses firmware over USB — no J-Link required.",
    },
]

TEMPLATE = """<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <meta name="robots" content="index, follow">
    <title>{title} — RGB Sunglasses</title>
    <meta name="description" content="{desc}">
    <link rel="canonical" href="https://rgb-sunglasses.autom8ed.com/{out}">
    <link rel="stylesheet" href="/assets/style.css">
</head>
<body>
<nav class="nav">
    <div class="nav-inner">
        <a class="brand" href="/"><span class="brand-dot"></span>RGB Sunglasses</a>
        <div class="nav-links">
{navlinks}
        </div>
    </div>
</nav>

<main class="prose">
{content}
</main>

<footer>
    <span>© 2026 RGB Sunglasses</span>
    <span>
        <a href="/">Home</a> ·
        <a href="mailto:privacy@autom8ed.com">Contact</a> ·
        <a href="https://github.com/skalldri/rgb-sunglasses" target="_blank" rel="noopener">GitHub</a>
    </span>
</footer>
</body>
</html>
"""


def render_nav():
    rows = []
    for href, label, external in NAV_LINKS:
        attr = ' target="_blank" rel="noopener"' if external else ""
        rows.append(f'            <a href="{href}"{attr}>{label}</a>')
    return "\n".join(rows)


def build():
    # superfences (from pymdown-extensions) renders fenced code correctly even
    # when nested inside lists — plain fenced_code does not, which matters for
    # step-by-step docs. tables/sane_lists/toc cover the rest.
    md = markdown.Markdown(
        extensions=["pymdownx.superfences", "tables", "toc", "sane_lists"]
    )
    nav = render_nav()
    for doc in DOCS:
        src = os.path.join(REPO_ROOT, doc["src"])
        with open(src, encoding="utf-8") as f:
            body = md.convert(f.read())
        md.reset()
        html = TEMPLATE.format(
            title=doc["title"],
            desc=doc["desc"],
            out=doc["out"],
            navlinks=nav,
            content=body,
        )
        out_dir = os.path.join(SITE_DIR, doc["out"])
        os.makedirs(out_dir, exist_ok=True)
        out_path = os.path.join(out_dir, "index.html")
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(html)
        print(f"rendered {doc['src']} -> site/{doc['out']}/index.html ({len(html)} bytes)")


if __name__ == "__main__":
    build()
