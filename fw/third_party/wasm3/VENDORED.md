# Wasm3 vendoring record

- Upstream: https://github.com/wasm3/wasm3
- Revision: `8815edc280e6fb039dbdc40dbb4cdebd20d769f5`
- Retrieved: 2026-08-14
- License: MIT, preserved in `LICENSE`
- Upstream archive SHA-256: `31be9cfd655879d5c5e9a5067f8e964d70d8ea7e0ea3a38d32c5ace8d163aa92`
- `source/` plus `LICENSE` manifest SHA-256: `fb4b94483840bd9c5aa84f93fdec70433d8a5bb524662c107a133b2db503fdb0`

The `source/` directory and `LICENSE` are copied from the pinned upstream
revision. The parent `CMakeLists.txt` is project-owned integration code and
builds only the core interpreter. WASI and the optional libc bindings are not
linked into the firmware.

`node fw/scripts/tests/test_wasm3_vendor.mjs` recomputes the local manifest
digest without network access and verifies that this record still names the
expected upstream revision and archive digest.

To update, review the upstream diff from the revision above, replace `source/`
and `LICENSE` from one exact commit, update this record, then rerun the firmware
build, module contract test, memory attribution, and hardware smoke test.
