# Vendored files in this directory

These four files are copied verbatim from **jothepro/doxygen-awesome-css**,
license MIT (see `LICENSE.doxygen-awesome`):

- `doxygen-awesome.css` — the base theme
- `doxygen-awesome-sidebar-only.css` — sidebar layout (paired with
  `GENERATE_TREEVIEW = YES` / `FULL_SIDEBAR = NO` in `../Doxyfile`)
- `doxygen-awesome-sidebar-only-darkmode-toggle.css` +
  `doxygen-awesome-darkmode-toggle.js` — the light/dark switch. Not
  cosmetic-optional: the rest of the site themes itself from the OS
  `prefers-color-scheme` (`site/assets/style.css`), and without these `/api`
  would stay light while `/docs` went dark. The toggle follows the OS setting
  by default and stores an explicit choice in localStorage; it applies
  `dark-mode` to `<html>`, which is what the nav styles in `header.html` key
  off.

| Field | Value |
| --- | --- |
| Upstream | https://github.com/jothepro/doxygen-awesome-css |
| Tag | `v2.3.4` |
| Tarball | https://github.com/jothepro/doxygen-awesome-css/archive/refs/tags/v2.3.4.tar.gz |

They are vendored rather than fetched at build time so the docs build has no
network dependency — the same reasoning behind the pinned toolchain composite
actions under `.github/actions/`.

## Doxygen version compatibility

Upstream states v2.3.4 "works best with doxygen 1.9.1 - 1.9.4 and 1.9.6 -
1.12.0" (note 1.9.5 is excluded). The docs build pins **Doxygen 1.12.0** — the
top of that supported range — in both `.github/actions/doxygen/action.yml` (CI
gate + Pages deploy) and `.devcontainer/Dockerfile` (local preview), so a
`WARN_AS_ERROR` build behaves identically everywhere.

Upstream also requires `HTML_COLORSTYLE = LIGHT` on Doxygen >= 1.9.5; that is
set in `../Doxyfile`. Do not remove it while the pinned version is >= 1.9.5.

## Upgrading

1. Download the new tag's tarball, replace the four files above and `LICENSE.doxygen-awesome`.
2. Check the new README's supported-Doxygen range against the pinned version in
   `.github/actions/doxygen/action.yml`; bump both together if they no longer overlap.
3. Update the tag in the table above.
4. Rebuild locally (see `../README.md`) and confirm zero warnings before pushing —
   `sdk-ci.yml`'s `docs` job treats warnings as errors.

`header.html` in this directory is **ours**, not upstream. It carries the site
nav bar so `/api` matches the rest of https://rgb-sunglasses.autom8ed.com, and
it overrides the theme's desktop layout (see the long comment in the file):
upstream positions the sidebar with a single `--top-height` constant that
assumes the doxygen header starts at the top of the viewport and has a fixed
height, and our nav bar plus a variable-length `PROJECT_NUMBER` break both
assumptions. The override replaces those offsets with a CSS grid so the
browser sizes the header instead.

It was derived from Doxygen 1.12.0's built-in template
(`doxygen -w html header.html footer.html stylesheet.css`); regenerate from the
same command if a Doxygen upgrade changes the expected `$placeholders`, then
re-apply the two "RGB Sunglasses site" blocks. There is deliberately no
`HTML_FOOTER` — the stock footer is fine, and a custom one only duplicated the
nav bar's links.
