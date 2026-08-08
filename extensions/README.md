# Community extension registry

This directory is the registry of **community rgbx animation extensions** —
extensions developed in standalone repos (from
[rgbx-extension-template](https://github.com/skalldri/rgbx-extension-template))
and shipped on this repo's firmware releases. Design:
`fw/docs/standalone-extension-repos.md`. The worked example is
[rgbx-demo-wave](https://github.com/skalldri/rgbx-demo-wave).

## Publishing your extension

1. Develop it from the template (see the template README: build with
   `./build.sh`, test by dragging the `.wasm` onto
   <https://rgb-sunglasses.autom8ed.com/sim/>).
2. Open a PR against this repo adding one entry to `registry.json`:
   - `name` — must equal your CMake `project()` name and match
     `^[a-z0-9_]{1,25}$` (it becomes the `.llext` filename on the device)
   - `repo` — your public GitHub repo
   - `rev` — the full 40-hex commit SHA to publish (never a branch/tag;
     changing it later means another reviewed PR)
   - `description`, `author`, `license` (OSI-approved; your repo must carry
     the license file)
3. CI (`community-extensions.yml`) validates the registry and builds your
   pinned commit from source — both targets, pinned toolchains, all gates
   (undefined symbols vs the device's export table, section layout, heap
   fit, wasm import/export contract).
4. A maintainer reviews the PR. **The repo + rev is the trust decision** —
   CI green is the floor, not the bar.

Once merged, every `fw-v*` firmware release rebuilds your extension from its
pinned rev against that release's SDK and attaches `<name>.llext` as a
release asset. The companion app then installs it onto devices automatically
(it syncs every `.llext` asset on the latest firmware release). A build
failure at release time (e.g. after an ABI bump) drops your extension from
that release — visibly, in the release workflow summary — without blocking
the firmware; fix and PR a new rev to ride the next release.

## Local validation

```bash
node extensions/validate-registry.mjs
```
