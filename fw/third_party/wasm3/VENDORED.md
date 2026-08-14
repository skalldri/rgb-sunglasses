# Wasm3 vendoring record

- Upstream: https://github.com/wasm3/wasm3
- Revision: `8815edc280e6fb039dbdc40dbb4cdebd20d769f5`
- Retrieved: 2026-08-14
- License: MIT, preserved in `LICENSE`
- Upstream archive SHA-256: `31be9cfd655879d5c5e9a5067f8e964d70d8ea7e0ea3a38d32c5ace8d163aa92`
- `source/` plus `LICENSE` manifest SHA-256: `fb4b94483840bd9c5aa84f93fdec70433d8a5bb524662c107a133b2db503fdb0`

The `source/` directory began as an exact copy from the pinned upstream
revision. The project maintains one reviewed Zephyr port patch in
`source/m3_core.c`: Wasm3's fixed allocator now has fail-without-consumption,
bulk-reset, and high-water semantics, and its terminal abort becomes a
sandbox-contained Zephyr oops. The parent `CMakeLists.txt` routes all mutable
library globals into the Wasm application-memory partition and builds only the
core interpreter. WASI and the optional libc bindings are not linked into the
firmware.

`node fw/scripts/tests/test_wasm3_vendor.mjs` recomputes the local manifest
digest without network access and verifies that this record still names the
expected upstream revision and archive digest.

To update, review the upstream diff from the revision above, replace `source/`
and `LICENSE` from one exact commit, reapply and review the Zephyr port patch,
update this record, then rerun the firmware build, module contract test, memory
attribution, sandbox fault tests, and hardware smoke test.
