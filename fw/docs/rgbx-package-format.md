# RGBX package format version 1

An `.rgbx` file is the future loadable WebAssembly extension format. Version 1
is deliberately rigid: one fixed envelope, one fixed-position canonical CBOR
manifest, one WebAssembly module, and one integrity digest. It contains no
native code and grants no filesystem, networking, BLE, kernel, or WASI access.

Package parsing alone does not authorize installation or execution. Firmware
must separately enforce the manifest policy, inspect the WebAssembly feature and
import surface, fully compile it inside the unprivileged sandbox, and complete
the staged installation transaction.

## Binary envelope

All header integers are unsigned little-endian values.

| Offset | Size | Field | Version 1 rule |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | ASCII `RGBX` |
| 4 | 2 | container version | `1` |
| 6 | 2 | header size | `20` |
| 8 | 4 | manifest size | `1..2048` bytes |
| 12 | 4 | Wasm size | `8..8192` bytes, further bounded by firmware policy |
| 16 | 4 | flags | `0` |
| 20 | manifest size | manifest | Canonical CBOR array below |
| next | Wasm size | module | Standard Wasm magic and version 1 |
| final | 32 | package digest | SHA-256 of every preceding package byte |

The total file size must equal the sum of these regions exactly. Truncation,
overflow, padding, concatenated data, and unknown flags all fail admission. The
digest is an integrity check, not proof of who authored the package. Package
signing and local-owner trust policy are separate decisions.

## Manifest

The manifest is a definite-length CBOR array with exactly 15 fields:

```text
[
  1,                              // manifest schema version
  extension_id,                   // stable lowercase token
  display_name,                   // printable ASCII
  [major, minor, patch],           // uint16 semantic version components
  rgbx_abi,
  minimum_firmware_abi,
  [display_width, display_height],
  capability_bits,
  memory_max_bytes,
  execution_budget_class,
  source_language_id,
  compiler_id,
  compiler_version,
  source_digest,                  // empty bstr or exactly 32 bytes
  parameters
]
```

The fixed array is intentional. The pinned zcbor decoder can enforce definite
lengths and minimally encoded values at runtime, but it does not enforce the
canonical ordering of map keys. A positional schema removes that ambiguity and
makes field order part of the schema while rejecting unknown, missing, or extra
items.

Version 1 strings are printable ASCII. The extension id is 1 to 31 bytes, begins
with a lowercase letter or digit, and thereafter contains only lowercase
letters, digits, `.`, `_`, or `-`. Display names are 1 to 31 bytes. Parameter
names are 1 to 19 bytes. Source-language ids and compiler versions are 1 to 15
bytes; compiler ids are 1 to 31 bytes. Unicode is deferred until the product has
an explicit normalization and confusable-identity policy.

Capability bits are:

| Bit | Capability |
| ---: | --- |
| 0 | button snapshots |
| 1 | IMU snapshots |
| 2 | audio snapshots |

Drawing primitives are part of the RGBX ABI rather than a requested sensor
capability. Unknown or host-disallowed bits fail admission.

Each parameter is a definite-length three-item array:

```text
[type, name, default]
```

Types match the existing product metadata model: `0` is uint32, `1` is a
24-bit `0x00RRGGBB` color, `2` is a CBOR boolean, and `3` is a printable ASCII
string of at most 31 bytes. A package has at most 16 parameters, at most four of
which may be strings, and parameter names must be unique within the package.

## Validation order

Firmware admission follows this order and leaves caller output unchanged on any
failure:

1. Validate the fixed header and section sizes without forming untrusted
   pointers.
2. Require the exact total file size.
3. Verify SHA-256 over all bytes before the digest trailer.
4. Decode the manifest with canonical enforcement and require full consumption.
5. Enforce identity, ABI, firmware, geometry, capability, memory, budget,
   compiler-metadata, source-digest, and parameter policy.
6. Require the standard eight-byte Wasm magic and version header.
7. In the later runtime admission stage, inspect imports, exports, Wasm features,
   functions, globals, locals, memory, metering, and fully compile the module in
   the `K_USER` Wasm sandbox.

## SHA-256 adapter

`rgbx_package::verifySha256Psa()` is the production crypto adapter for the
parser callback. It calls PSA `psa_hash_compare()` directly and maps every PSA
error or digest mismatch to a closed admission failure. The platform must have
initialized PSA Crypto before validation starts; the adapter does not initialize
crypto, allocate memory, log package contents, or provide a fallback hash.

The adapter and parser currently compile only in the dedicated ARM QEMU test
image. That image checks the standard SHA-256 `abc` vector, admits a genuinely
SHA-sealed package, and rejects changes in the manifest, Wasm payload, or digest
trailer.

The first parser implementation is intentionally not linked into production.
The dual-runtime image is too close to its flash and RAM stop-loss. Filesystem
loading and execution remain disabled until an independently reviewed promotion
packet either recovers enough measured capacity or retires LLEXT.

## Production promotion checklist

- Independently review integer bounds, pointer formation, canonical-CBOR
  behavior, digest coverage, policy enforcement, and parser stack use.
- Copy the candidate into one bounded staging buffer and validate that immutable
  snapshot. Do not validate one file view and execute another.
- Require exact read length and file size before parsing. Short reads, growth,
  replacement, padding, and concatenated packages fail the transaction.
- Enable canonical zcbor support only with the reviewed production parser wiring
  and attribute its measured flash and RAM cost.
- Inspect and fully compile the Wasm module inside the unprivileged sandbox before
  changing the registry, active extension, persistent settings, or installed-file
  state.
- Keep package bytes and digests out of ordinary logs. Report only bounded result
  codes and the already-reviewed extension identity after successful admission.
- Preserve at least 64 KiB free app-core flash and 24 KiB free app-core RAM, or
  retire enough LLEXT code and data to restore those margins before enabling the
  path.

## LLEXT retirement

LLEXT is a migration mechanism, not a permanent second extension platform.
After one planned compatibility release and conversion of every supported
extension, the project removes the native loader, Zephyr LLEXT and EDK support,
the 24 KiB native heap, native sandbox, exported native symbol surface, ARM SDK
artifacts, `.llext` release jobs and fixtures, and app or registry installation
branches for native extensions.

The first Wasm-only firmware stops scanning or executing `.llext` files but
retains existing files through the bounded rollback window. File cleanup is a
later explicit user-approved action. Reclaimed memory first restores the agreed
firmware margins; any remaining capacity is assigned through reviewed limits
rather than silently increasing the Wasm arena or module profile.
