# Community extension registry

This directory is the registry of **community rgbx animation extensions** —
extensions developed in standalone repos (from
[rgbx-extension-template](https://github.com/skalldri/rgbx-extension-template))
and shipped on this repo's firmware releases. The worked example is
[rgbx-demo-wave](https://github.com/skalldri/rgbx-demo-wave); the design
rationale is in
[`fw/docs/standalone-extension-repos.md`](https://github.com/skalldri/rgb-sunglasses/blob/main/fw/docs/standalone-extension-repos.md).

## Publishing your extension

1. Develop it from the template (see the
   [template README](https://github.com/skalldri/rgbx-extension-template#readme):
   build with
   [`./build.sh`](https://github.com/skalldri/rgbx-extension-template/blob/main/build.sh),
   test by dragging the `.wasm` onto
   <https://rgb-sunglasses.autom8ed.com/sim/>).
2. Open a PR against this repo adding one entry to
   [`registry.json`](https://github.com/skalldri/rgb-sunglasses/blob/main/extensions/registry.json):
   - `name` — must equal your CMake `project()` name and match
     `^[a-z0-9_]{1,25}$` (it becomes the `.llext` filename on the device)
   - `repo` — your public GitHub repo
   - `rev` — the full 40-hex commit SHA to publish (never a branch/tag;
     changing it later means another reviewed PR)
   - `description`, `author`, `license` (OSI-approved; your repo must carry
     the license file)
3. CI ([`community-extensions.yml`](https://github.com/skalldri/rgb-sunglasses/blob/main/.github/workflows/community-extensions.yml))
   validates the registry and builds your
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

([`validate-registry.mjs`](https://github.com/skalldri/rgb-sunglasses/blob/main/extensions/validate-registry.mjs))

## Before you submit: bound your phase accumulators

Nothing in CI can catch this, so it is on you and on the reviewer.

If your extension calls `sinf`/`cosf`/`tanf` on a phase that accumulates every tick,
**wrap that accumulator**. The device's libm is cheap only while `|x| <= 201.06` and
gets continuously more expensive above it, so an unwrapped accumulator makes an
extension run at full speed for a minute or two and then degrade — invisibly to a
short test, because the accumulator resets every time the extension is activated.

This is not hypothetical: the first two extensions in this registry shipped with it. Issue
[#304](https://github.com/skalldri/rgb-sunglasses/issues/304) — plasma's per-tick cost
climbed 3.4 ms -> 25 ms over five minutes and missed essentially every frame.

Full explanation, the wrap idiom, the `fmodf` sign trap, and how to soak-test it:
**"Bound your phase accumulators"** in
[`fw/extensions/README.md`](../fw/extensions/README.md).
