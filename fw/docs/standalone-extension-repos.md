# Standalone extension repos: SDK artifact, template repo, and registry

Status: **implemented** — all five phases are complete (§10). This started life
as a design doc and is kept as the reference for *why* the SDK, template repo
and registry are shaped the way they are; §10 records what shipped in which PR,
including the two acceptance criteria deliberately deferred to a hardware
session and to the first post-merge `fw-v*` release (issues #295 and #298).

## 1. Motivation and goals

The WASM simulator (`fw/sim/`, hosted at <https://rgb-sunglasses.autom8ed.com/sim/>)
already lets anyone run an rgbx animation extension without hardware, including
drag-and-dropping an arbitrary `.wasm` build. What's missing is a way to *develop*
an extension without cloning this monorepo, and a way to get a finished extension
onto other people's devices. This design closes both gaps, as a step toward an
eventual "extension store":

**Developer flow**

1. Fork the `rgbx-extension-template` repo (its example source is the Hello
   kitchen-sink extension).
2. Implement the animation; `./build.sh` produces both a simulator `.wasm`
   and a device `.llext`.
3. Test by dragging the `.wasm` onto the hosted simulator. Optionally USB-copy
   the `.llext` to a device's `/NAND:/ext/` for on-hardware testing.
4. Submit a PR to this repo adding one entry to `extensions/registry.json`.
5. On the next `fw-v*` release, release CI rebuilds the extension from its pinned
   SHA and attaches the `.llext` as a release asset — at which point the
   companion app's existing extension sync (`app/services/extension-sync.ts`)
   installs it onto devices **with zero app changes**, because it already syncs
   every `*.llext` asset on the firmware release.

**Goals**: no Zephyr/west/NCS requirement for extension developers; one pinned,
reproducible toolchain per side; every failure that used to appear as a silent
on-device load fault caught at build time instead.

## 2. Non-goals (v1)

- No `rgbx-sim` CLI publishing (headless scenarios/goldens stay in-repo);
  simulator testing from a fork is drag-and-drop only.
- No signing, provenance, or automated malware analysis of extensions.
- No store UI and no third-party release sources in the app — distribution is
  exclusively via this repo's release assets.
- No change to the in-repo extension build (`fw/extensions/build.sh`, EDK path)
  — dogfooding the SDK for in-repo extensions is an optional later phase.

## 3. Architecture overview

```
 rgbx-extension-template (fork)          rgb-sunglasses (this repo)
 ┌────────────────────────────┐          ┌─────────────────────────────────┐
 │ src/main.c                 │          │ fw/include/rgbx/*.h  (ABI)      │
 │ CMakeLists.txt ──sdk download──────►  │ fw/sim/shim/*        (wasm shim)│
 │ cmake/fw-release.cmake     │  pinned  │ fw/sdk/**            (arm shim, │
 │   (fw release + sha256 pin)│  rgbx-sdk│   cmake pkg, gates, packaging)  │
 │ CMakePresets.json          │  tarball │        │ package-sdk.sh         │
 └──────────┬─────────────────┘          │        ▼                        │
            │ ./build.sh                 │ rgbx-sdk-<ver>.tar.gz ──────────┼──► fw-v* release asset
            ▼                            │                                 │
   build/wasm/<name>.wasm ──drag──►  hosted sim (/sim)                     │
   build/arm/<name>.llext            (zero-import sandbox)                 │
            ▲                            │ extensions/registry.json ◄──────┼── community PR
            │                            │        │ community-extensions CI│
            └── same build, run by ──────┼────────┘ (clone @ SHA, build,   │
                registry CI at release   │          gates, attach .llext)  │
                                         └─────────────────┬───────────────┘
                                                           ▼
                                          app extension-sync → /NAND:/ext/ on device
```

## 4. Why no Zephyr toolchain is needed (verified)

The historical assumption was that building an `.llext` requires the Zephyr SDK
plus a configured proto0 build (for the `llext-edk` archive). Code reading plus a
verification experiment (§4.1) shows the actual surface is tiny:

- The ABI headers `fw/include/rgbx/rgbx_api.h` / `rgbx_animation.h` are pure
  C/C++ with no Zephyr includes beyond `<zephyr/llext/symbol.h>`.
- `EXPORT_SYMBOL` under `-DLL_EXTENSION_BUILD` expands to a single
  `struct llext_const_symbol` entry in section `.exported_sym`
  (NCS `zephyr/include/zephyr/llext/symbol.h`) — reproducible as a ~40-line
  standalone shim header with no transitive Zephyr includes.
- `<zephyr/kernel.h>` is only needed for the `printk` declaration — a shim that
  forwards to `<rgbx/rgbx_sys.h>`, where the SDK single-sources the declarations
  for the whole sanctioned symbol surface. It used to carry its own prototype,
  which is how it came to disagree with the simulator shim's (issue #351).
- The EDK's compile flags are deliberately host-path-free (Zephyr's
  `llext-edk.cmake` strips sysroot and prefix-map flags); everything that
  matters is generic GCC: `-mcpu=cortex-m33 -mthumb -mfloat-abi=hard
  -mfpu=fpv5-sp-d16 -mlong-calls -DLL_EXTENSION_BUILD` (plus
  `-std=c++23 -fno-exceptions -fno-rtti` for C++).
- The mandatory `ld -r` partial link (C++ COMDAT region-overlap fix, see
  `fw/extensions/build.sh`) is stock GNU binutils behavior.

The *real* constraint is not the toolchain but the device's import table: at
llext load, the only symbols an extension may reference are the Zephyr llext
exports `strcpy strncpy strlen strcmp strncmp memcmp memcpy memset` plus
`printk` (`zephyr/subsys/llext/llext_export.c`, `lib/os/printk.c`). No
`__aeabi_*` libgcc helpers and no libm are exported, and whether the compiler
emits a helper call (soft-float, 64-bit division) or an inline instruction is a
codegen decision that varies by compiler version and flags. Two consequences:

1. **The SDK must gate on undefined symbols** (§5, `check-llext.sh`): `nm -u`
   over the produced `.llext`, fail on anything outside the allowed set. This
   converts the entire "works in sim, silently fails to load on device" class
   (the documented `sinf()` trap, and quieter variants) into a build error.
2. **The toolchain must be pinned for reproducible codegen** — not because
   Zephyr's compiler is special, but so template CI and registry CI produce the
   same undefined-symbol set and the same code.

### 4.1 Phase 0 verification experiment (run 2026-08-08, devcontainer)

Compiled `fw/extensions/hello/hello.c` (C path) and
`fw/extensions/plasma/plasma.cpp` (C++ wrapper path — that file has since
been renamed to `fw/extensions/cpptest/cpptest.cpp`, and the production
Plasma moved to the registry-shipped rgbx-plasma repo; the `plasma.llext`
names below are the historical record of this run) with **only** the flag set
above plus two shim headers (`zephyr/llext/symbol.h`, `zephyr/kernel.h` —
prototypes of the SDK's `arm/shim/`), no EDK, no Zephyr include tree, using
Zephyr SDK 0.17.0's GCC 12.2.0 / GNU ld 2.38, then `ld -r`:

- Both compile and partial-link cleanly. `hello.llext` 3096 B, `plasma.llext`
  3464 B (comfortably inside the 24 KB llext heap).
- `readelf -S` on both: `.exported_sym` present (6 entries × 8 B); alloc
  sections laid out `.text` → COMDAT `.text._Z*` (plasma) → `.rodata` →
  `.exported_sym` → `.data` → `.bss` with monotonically increasing file
  offsets — the layout the on-device region-overlap check requires.
- `nm -u hello.llext` → `memset`, `printk` only. Notably hello.c never calls
  `memset` — GCC synthesized it from the framebuffer-clearing loop, which is
  the codegen-dependent-import point of §4 demonstrated live, and it lands in
  the allowed set.
- `nm -u plasma.llext` → empty.
- Negative test: a TU calling `sinf()` and doing `uint64_t` division builds to
  an object whose `nm -u` reports exactly `sinf` and `__aeabi_uldivmod` — the
  gate's grep catches both.

**Residual risks, deferred to Phase 1** (each expected-benign, none blocking
the design): (a) an actual on-device load of a shim-built `.llext` (the loader
consumes name + `.exported_sym` entries; the shim reproduces the struct
byte-for-byte and static-asserts its size); (b) repeating the experiment with
the pinned Arm GNU Toolchain build of GCC rather than Zephyr SDK's (same GCC +
binutils lineage; `ld -r` ordering is an implementation behavior, so
`check-llext.sh` keeps the `readelf` layout check as a permanent guard rather
than trusting any toolchain); (c) `<type_traits>` / freestanding-libc header
availability in the Arm GNU toolchain (both known-present).

## 5. `rgbx-sdk` release artifact

One tarball, attached to every `fw-v*` release. **SDK version == firmware
release version** — there is no separate SDK version stream, so "which SDK goes
with which firmware" can never be asked.

```
rgbx-sdk-<fw-version>/
  sdk-manifest.json          { "sdkVersion", "fwRelease", "abiVersion",
                               "armToolchain": "arm-gnu-13.2.Rel1", "wasiSdk": "33.0" }
  include/rgbx/rgbx_api.h    copied verbatim from fw/include/rgbx/
  include/rgbx/rgbx_animation.h
  include/rgbx/rgbx_sys.h    declarations for the allowed-symbols surface, so an
                             author never hand-writes a prototype (issue #351)
  arm/
    shim/include/zephyr/llext/symbol.h   EXPORT_SYMBOL -> .exported_sym entry (new, §4)
    shim/include/zephyr/kernel.h         forwards to <rgbx/rgbx_sys.h>
    allowed-symbols.txt                  strcpy strncpy strlen strcmp strncmp
                                         memcmp memcpy memset printk
    check-llext.sh                       nm -u gate against allowed-symbols.txt
                                         + readelf -S layout check (§4.1 risk b)
                                         + alloc-section size vs 24 KB llext heap
  wasm/
    shim/include/...                     copied from fw/sim/shim/include/
    shim/sim_shim.c                      copied from fw/sim/shim/ (printk -> log buffer)
    shim/abi_offsets.c                   copied — struct-layout static_asserts compile
                                         into every module; ABI drift fails the build
    check-wasm.mjs                       copied from fw/sim/scripts/ (zero-import +
                                         required-export gate; needs Node)
  cmake/
    rgbx-sdk-config.cmake                package entry point; defines rgbx_add_extension()
    toolchains/arm-llext.cmake           arm-none-eabi toolchain file with the §4 flags;
                                         warns on unpinned compiler version, FATAL_ERROR
                                         under -DRGBX_STRICT_TOOLCHAIN=ON (CI sets it)
    toolchains/wasm.cmake                wasi-sdk clang toolchain file
  scripts/
    install-arm-toolchain.sh             download pinned Arm GNU Toolchain to
                                         ~/.cache/rgb-sunglasses, print its root
    install-wasi-sdk.sh                  self-contained copy of
                                         fw/sim/scripts/install-toolchain.sh
```

`rgbx_add_extension(<name> SOURCES <one .c or .cpp>)` branches on `RGBX_TARGET`
(set by whichever toolchain file configured the build):

- **arm**: compile the TU → mandatory `ld -r` custom command → `.llext` →
  POST_BUILD `check-llext.sh`.
- **wasm**: `add_executable` of the TU + `sim_shim.c` + `abi_offsets.c` with
  `-O2 -mexec-model=reactor` and `-Wl,--export-if-defined=` for the five
  required rgbx symbols + `rgbx_good_moment` + `rgbx_sim_log_buf`/`_len`
  (mirroring `fw/sim/build-extensions.sh`) → `.wasm` → POST_BUILD
  `check-wasm.mjs`.

The SDK ships **no build step of its own** — shim sources compile inside the
consuming project under that project's toolchain. The two-toolchain problem is
solved by presets in the template (§6), not by a superbuild.

**Consumption mechanism (implementation amendment):** the template downloads
and extracts the SDK **before `project()`** via `file(DOWNLOAD ...
EXPECTED_HASH SHA256=...)` + `file(ARCHIVE_EXTRACT)`, then sets
`CMAKE_TOOLCHAIN_FILE` from the extracted tree and `include()`s
`cmake/rgbx-sdk-config.cmake` after `project()`. Literal `FetchContent` cannot
work here: the SDK's toolchain files must exist before `project()` runs, but
`FetchContent` executes after it. Same spirit (CMake-native download pinned by
URL + sha256), single configure pass. CI and local overrides supply a
pre-extracted tree via `-DRGBX_SDK_SOURCE_DIR=<path>` (pointing at a
`package-sdk.sh` output — not at `fw/sdk/`, which is only a source fragment
of the assembled SDK). The ARM toolchain file pins **`-O2`** explicitly: the
undefined-symbol surface is codegen-dependent, and notably the in-repo EDK
path compiles extensions with no `-O` flag at all (`-Os` is stripped by
Zephyr's `LLEXT_REMOVE_FLAGS` with nothing re-added), so inheriting a build
type would silently vary the gate result.

Monorepo side: new `fw/sdk/` directory holds the files that don't already
exist elsewhere (arm shims, `check-llext.sh`, `allowed-symbols.txt`, the cmake
package, `install-arm-toolchain.sh`, `package-sdk.sh`). `package-sdk.sh`
assembles the tarball by copying from the canonical locations
(`fw/include/rgbx/`, `fw/sim/shim/`, `fw/sim/scripts/`) so nothing is
duplicated in-tree. `.github/workflows/release.yaml` gains a step to run it and
attach the tarball; `.github/workflows/build.yaml` uploads it as a CI artifact
(pre-release testing).

## 6. Template repo: `rgbx-extension-template`

A new GitHub repo under the same owner, marked as a GitHub *template
repository* (fork or "Use this template" both work).

```
CMakeLists.txt            pre-project() download+extract of the SDK tarball
                          (URL/hash from cmake/fw-release.cmake, see §5's
                          consumption amendment) + rgbx_add_extension()
CMakePresets.json         version-3 presets (works on CMake 3.21, which is
                          what the devcontainer has): configure/build presets
                          "arm" and "wasm" (separate binary dirs, each
                          selecting the SDK toolchain file via RGBX_TARGET)
build.sh                  wrapper running both presets in sequence, yielding
                          build/arm/<name>.llext AND build/wasm/<name>.wasm
                          (no `cmake --workflow` — that needs CMake >= 3.25)
cmake/fw-release.cmake    set(RGBX_FW_RELEASE "fw-v1.4.0")
                          set(RGBX_SDK_SHA256 "<sha256 of the release asset>")
                          — the single pin; bumping = editing these two lines
src/main.c                copy of fw/extensions/hello/hello.c (kitchen-sink
                          reference: every param type, every input source)
README.md                 the full flow: fork -> rename -> build -> drag the
                          .wasm onto https://rgb-sunglasses.autom8ed.com/sim/
                          -> (optional) USB-copy the .llext to /NAND:/ext/
                          -> PR a registry.json entry to rgb-sunglasses
.github/workflows/ci.yml  ubuntu runner: node 20, cached pinned toolchains via
                          the SDK's install scripts, then
                          ./build.sh -DRGBX_STRICT_TOOLCHAIN=ON;
                          uploads both artifacts
.gitignore                build/
LICENSE                   MIT (registry entries must carry an OSI license)
```

Constraints inherited from the extension model (enforced by the SDK gates, and
documented in the template README): single translation unit; 40×12 framebuffer;
≤16 params, ≤4 string params; no heap, no exceptions, no RTTI, no libm; render
near full-scale 255 (global brightness factor is 0.02); globals reset on every
activation.

## 7. Registry: `extensions/registry.json`

Location is deliberately **outside `fw/`** so registry PRs never trigger the
firmware build (`build.yaml` gates on `fw/**`) — that path split is the
failure-isolation seam between community code and firmware CI.

```json
{
  "version": 1,
  "extensions": [
    {
      "name": "plasma_storm",
      "repo": "https://github.com/alice/rgbx-plasma-storm",
      "rev": "<full 40-hex commit SHA>",
      "description": "Audio-reactive plasma variant",
      "author": "alice",
      "license": "MIT"
    }
  ]
}
```

Validation (`extensions/validate-registry.mjs`, run in CI and locally): unique
names matching `[a-z0-9_]{1,25}` (the name becomes the `.llext` filename on
`/NAND:/ext/`, and slot discovery is filename-sorted; the cap is 25, not the
firmware's 31-char `kMaxNameLen`-derived limit, because the app-side
`MAX_EXTENSION_NAME_LENGTH = 31` in `app/services/extension-sync.ts` applies
to the whole asset filename *including* the 6-char `.llext` suffix — a longer
name would be silently skipped by extension sync); no collision with
in-repo extensions (currently `hello`, `cpptest` — read dynamically from `fw/extensions/`); `https://github.com/` repo URLs; full
40-hex `rev` (a pinned SHA, never a branch or tag — post-review tampering with
the source repo is inert; changing `rev` requires a new reviewed PR); `license`
required.

New workflow `.github/workflows/community-extensions.yml`, two triggers:

1. **`pull_request` on `extensions/registry.json`** — the gate for
   registry-addition/bump PRs. Validate script → generate a job matrix from the
   entries (`fail-fast: false` so one broken extension never hides another) →
   per entry: shallow-fetch the repo at `rev` → build both targets **against
   this checkout's own SDK** (point `FETCHCONTENT_SOURCE_DIR_RGBX_SDK` at a
   locally assembled `package-sdk.sh` output, overriding the extension's own
   pin) → `check-llext.sh` + `check-wasm.mjs`.
2. **`workflow_call` from `release.yaml`** — identical build at release time;
   an `attach-community` job with `if: always()` collects whichever per-entry
   artifacts succeeded and runs `gh release upload` for the `.llext` files.

Building against the *release's* SDK rather than the extension's own pin is
deliberate: every `.llext` shipped on release X is compiled against release X's
headers, so a release asset can never be ABI-stale. The extension's pin governs
only local development and its own template CI.

Isolation properties: the firmware build/release jobs never depend on the
community jobs; a failing community extension simply misses that release
(visible in the workflow summary) and rides again next release once fixed.

Review story for a registry PR: CI green is the floor (validate + both builds +
both gates per entry); the human maintainer review of `repo` + `rev` **is** the
trust decision (§9).

## 8. Toolchain choice and compatibility lifecycle

**ARM**: Arm GNU Toolchain (`arm-none-eabi-*`), one pinned release (candidate:
13.2.Rel1), used identically by template CI and registry CI, installed by the
SDK's `install-arm-toolchain.sh` (stable download URLs, ~100 MB). The Zephyr
SDK would also work (§4.1 used it) but adds nothing — no Zephyr headers are
consumed — and is a heavier install. The pin matters more than the brand: the
`nm` gate's result is codegen-dependent, so an identical compiler everywhere
makes "green in my fork's CI" predictive of "green in registry CI".
**WASM**: wasi-sdk pinned at 33.0, exactly as `fw/sim/` already does.

Compatibility chain:

- Root of compat is `RGBX_ABI_VERSION` (currently 1) in `rgbx_api.h`,
  documented append-only within a version: new *optional* exports are
  negotiated by symbol presence (like `rgbx_good_moment`), new manifest fields
  append. A newer SDK therefore builds older extension source unchanged.
- On an ABI bump to v2: the next release's SDK carries v2 headers; the
  release-time registry rebuild is the forcing function — every entry
  recompiles against v2 (the manifest's `abi_version` field is stamped from the
  header, so surviving entries are automatically v2), and entries that no
  longer compile visibly drop off that release. Template users update by
  bumping the two-line pin.
- Safety nets for anything stale: release assets can't be stale by
  construction (above); a locally built `.llext` against old headers is
  rejected at boot discovery with `BadAbiVersion`
  (`fw/src/extensions/extension_manifest.cpp`); wasm-side layout drift fails
  the build via `abi_offsets.c` static asserts before the sim ever sees it.

## 9. Security and trust (proportionate v1)

Where third-party code executes, and what contains it:

| Context | Containment |
|---|---|
| Extension dev's own fork CI | Their repo, their runner quota — not our concern |
| Registry CI (monorepo runners) | CMake configure of untrusted code is arbitrary code execution: the build jobs run with `permissions: {}` (zero-scope `GITHUB_TOKEN`), no secrets in env; artifacts hand off to a separate trusted attach job. Blast radius = runner minutes. |
| Browser (hosted sim) | Existing zero-import WebAssembly sandbox (`fw/sim/core/sandboxCore.ts`) |
| Device | Existing llext sandbox: K_USER thread + MPU domain, per-tick CPU budget, fault teardown/recovery, every manifest pointer bounds-checked before kernel dereference |

Plus: pinned SHAs in the registry (tamper-inert, re-review to change), sha256
pin on the SDK tarball in templates, versioned toolchain downloads. Explicit
v1 non-goals: signing, provenance attestation, automated analysis, namespace
policy beyond maintainer review.

## 10. Implementation phases

Phase 0 — verification experiment: **done** (§4.1).

**Phase 1 — SDK artifact: done** (PR #294). `fw/sdk/` + `sdk-ci.yml` + the
`release.yaml`/`build.yaml` wiring, plus two hardening items beyond the
original scope: `check-llext.sh` mirrors the loader's merged-region overlap
model and its multiple-NOBITS reject, and `build.yaml` CI-asserts
`allowed-symbols.txt` ⊆ the built firmware's export table
(`check-allowed-symbols.sh`). §4.1 residual risk (b)/(c) closed (hello/plasma
rebuilt clean under the pinned Arm GNU 13.2.Rel1, identical `nm -u` sets);
risk (a) — on-device load of a shim-built `.llext` — still **deferred to a
hardware session**, bundled with issue #295 (curated libm exports).

**Phase 2 — template repo: done.** `rgbx-extension-template` is live (GitHub
template repo, CI green); the SDK tarball was backfilled onto the `fw-v3.0.0`
release, and the full fork → download-pinned-SDK → build → drag-drop loop is
validated against it.

**Phase 3 — registry + CI: done** (PR #296). `extensions/` + 
`community-extensions.yml` + the draft-release/attach flow in `release.yaml`,
seeded with the `rgbx-demo-wave` repo (created from the template). Hardened
per review: env-indirected registry values + charset-tight repo URLs (script
injection), full-clone reachability check on pinned revs (fork-network SHA
attack), draft-until-attached releases (no incomplete-asset window). The
original acceptance criterion — registry PR → release asset → app
extension-sync → running on a device — is **deferred, not dropped**: the
never-yet-exercised release-time path (attach-community publish + app sync,
including the degraded case of a missing community asset) must be actively
verified on the first post-merge `fw-v*` release, tracked as **issue #298**.

**Phase 4 — docs rerouted: done** (this change). Remaining optional
dogfooding happened by another route: plasma moved OUT to its own
registry-shipped repo (rgbx-plasma, sinf-modernized), releases stopped
attaching in-repo-built `.llext` entirely (every shipped extension now comes
through the registry), and the in-repo extensions that remain — `hello` and
the `cpptest` C++ fixture that replaced plasma — are dev/debug tools built by
CI but never released.

Existing files modified along the way: `.github/workflows/release.yaml`,
`.github/workflows/build.yaml`, `fw/extensions/README.md` (its "third parties
use `llext-edk.tar.xz`" section is superseded by the template/SDK),
`fw/extensions/build.sh` (header comment pointing standalone developers at the
SDK), `.claude/skills/add-extension/SKILL.md` (route standalone/third-party
extension work to the template flow), root `CLAUDE.md` task-routing table +
`fw/CLAUDE.md` cross-references.

## 11. Open questions

- Cadence mismatch: an extension merged to the registry only ships on the next
  `fw-v*` release. Acceptable for v1 (releases are cheap to cut); a future
  "extensions-only release" or separate asset channel could decouple them.
- Should `community-extensions.yml`'s PR gate also run the extension's `.wasm`
  through a headless smoke scenario (the rgbx-sim CLI exists in-repo even if
  unpublished)? Cheap to add later; omitted from v1 scope.
- Template distribution of `rgbx_animation.h`'s C++ path requires the consuming
  compiler to provide `<type_traits>` (§4.1 risk c) — confirm in Phase 1 on the
  pinned Arm GNU Toolchain and document a C-only fallback if it ever breaks.
