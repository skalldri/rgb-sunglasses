---
paths:
  - "fw/include/rgbx/**"
  - "fw/sdk/**"
  - "fw/src/extensions/extension_exports.c"
  - "fw/extensions/build-docs.sh"
  - "fw/sim/build-extensions.sh"
  - ".github/workflows/sdk-ci.yml"
  - "fw/docs/standalone-extension-repos.md"
---

# rgbx ABI, SDK and exported-symbol lockstep

Loads when you read the rgbx headers, the SDK, or the base image's export table. Runtime sandbox
behaviour is `.claude/rules/fw-extensions.md`. **Standalone/community extensions** (developed
outside this repo against the released `rgbx-sdk`) are a separate flow — route via the root
`CLAUDE.md` task-routing table; don't restate the route here.

## API docs are a gated build

API docs are published at <https://rgb-sunglasses.autom8ed.com/api> and built locally with
`fw/extensions/build-docs.sh` — never a bare `doxygen Doxyfile`: the script fixes the CWD the
Doxyfile's relative paths depend on and asserts the pinned Doxygen version, because an older one
exits 0 while leaving the HTML header template's placeholders unsubstituted. The build is
warnings-as-errors and gated pre-merge by `.github/workflows/sdk-ci.yml`'s `docs` job, so
**anything added to `fw/include/rgbx/` needs a `@brief` plus `@param`/`@return` on every parameter
and return value, or CI fails**.

## Exported symbols: three files move in lockstep

The one firmware-side invariant that lives in this repo:

1. **`fw/sdk/scripts/check-allowed-symbols.sh`** (run by `build.yaml`) asserts the SDK's
   `fw/sdk/arm/allowed-symbols.txt` ⊆ the built firmware's export table — **deliberately
   one-directional**. REMOVING a base-image `EXPORT_SYMBOL` that the list sanctions breaks CI;
   ADDING one is NOT flagged — a new export only becomes usable by standalone extensions after a
   manual `allowed-symbols.txt` addition (otherwise the SDK gate keeps rejecting it even though the
   device would resolve it; issue #295's libm additions had to update both sides together).
2. **`fw/include/rgbx/rgbx_sys.h`** (issue #351) must DECLARE every sanctioned symbol with the
   correct prototype, asserted by **`fw/sdk/scripts/check-sys-header.sh`** (`sdk-ci.yml`) — it
   generates a typed function-pointer initialization per symbol and compiles it, so an undeclared
   symbol and a wrong prototype both fail, in both directions.
3. The export table itself (`fw/src/extensions/extension_exports.c`).

Shipping the list without prototypes was its own silent-failure class: `extern "C"` matches on the
name, so a wrong signature still links, and the consequences diverge by target — ARM/ELF resolves
by name alone (a wrong return type survives AAPCS, a wrong parameter type corrupts the call
silently), while wasm types calls by full signature and traps with `RuntimeError: unreachable` on
the first call. The wasm link passes `-Wl,--fatal-warnings` (both `fw/sim/build-extensions.sh` and
`fw/sdk/cmake/rgbx-sdk-config.cmake`) so wasm-ld's signature-mismatch warning is a build error;
there is no ARM equivalent, which is why the header — not the gate — is the fix.
