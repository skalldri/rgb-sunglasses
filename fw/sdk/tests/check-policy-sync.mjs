#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
//
// Drift gate for the RGBX v2 admission profile and the SDK's toolchain pins.
//
// Three parties decide whether a module is admissible: the firmware, the
// SDK's post-link gate, and the SDK's package builder. They agree only
// because the ABI header declares the profile once and everything else is
// derived from it. This test proves each link of that chain still holds:
//
//   header -> sdk-manifest.json   the packager copied every limit verbatim,
//                                 and the manifest describes the very header
//                                 it was generated from;
//   header -> firmware            the admission path binds each of its own
//                                 constants to the macro, and keeps the
//                                 build-time assertions that catch a limit
//                                 which grew a second definition.
//
// The SDK tools need no clause here: they hold no limits of their own, they
// read the manifest this test just validated.
//
// The same argument applies to the toolchain pins the manifest publishes as
// the SDK's provenance. They are only provenance if they name the archives
// the installers actually verify, so each one is compared back to the
// installer that owns it.
//
//   check-policy-sync.mjs <repo-root> <sdk-manifest.json>

import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { join, resolve } from "node:path";

// Manifest policy field -> ABI header macro that owns its value.
const POLICY_MACROS = {
  width: "RGBX_V2_WIDTH",
  height: "RGBX_V2_HEIGHT",
  pixelsPerSpan: "RGBX_V2_PIXELS_PER_SPAN",
  maxParams: "RGBX_V2_MAX_PARAMS",
  maxStringParams: "RGBX_V2_MAX_STRING_PARAMS",
  stringParamSize: "RGBX_V2_STRING_PARAM_SIZE",
  audioBandCount: "RGBX_V2_AUDIO_BAND_COUNT",
  audioDisplayBucketCount: "RGBX_V2_AUDIO_DISPLAY_BUCKET_COUNT",
  imuAxisCount: "RGBX_V2_IMU_AXIS_COUNT",
  maxDiagnosticsPerTick: "RGBX_V2_DIAGNOSTIC_COUNT",
  moduleMaxBytes: "RGBX_V2_MODULE_MAX_BYTES",
  maxFunctions: "RGBX_V2_MAX_FUNCTIONS",
  maxGlobals: "RGBX_V2_MAX_GLOBALS",
  maxLocalsPerFunction: "RGBX_V2_MAX_LOCALS_PER_FUNCTION",
  minImports: "RGBX_V2_MIN_IMPORTS",
  maxImports: "RGBX_V2_MAX_IMPORTS",
  maxParamCallsPerTick: "RGBX_V2_MAX_PARAM_CALLS_PER_TICK",
  maxInputCallsPerTick: "RGBX_V2_MAX_INPUT_CALLS_PER_TICK",
  sectionAllowedMask: "RGBX_V2_SECTION_ALLOWED_MASK",
  sectionRequiredMask: "RGBX_V2_SECTION_REQUIRED_MASK",
};

// Firmware constant -> ABI header macro it must be defined as. A literal here
// would be a second opinion about the profile, which is exactly the drift
// this gate exists to catch.
const FIRMWARE_BINDINGS = {
  kMaxV2Imports: "RGBX_V2_MAX_IMPORTS",
  kMinV2Imports: "RGBX_V2_MIN_IMPORTS",
  kMaxV2Globals: "RGBX_V2_MAX_GLOBALS",
  kMaxV2ParamCallsPerTick: "RGBX_V2_MAX_PARAM_CALLS_PER_TICK",
  kMaxV2InputCallsPerTick: "RGBX_V2_MAX_INPUT_CALLS_PER_TICK",
  kV2PixelsPerSpan: "RGBX_V2_PIXELS_PER_SPAN",
  kV2SectionAllowedMask: "RGBX_V2_SECTION_ALLOWED_MASK",
  kV2SectionRequiredMask: "RGBX_V2_SECTION_REQUIRED_MASK",
};

// Constants the runtime shares with its pre-v2 profile cannot be defined from
// a v2 macro, so they are tied to it by a build-time assertion instead.
const FIRMWARE_ASSERTIONS = [
  ["kMaxFunctions", "RGBX_V2_MAX_FUNCTIONS"],
  ["kMaxLocalsPerFunction", "RGBX_V2_MAX_LOCALS_PER_FUNCTION"],
  ["CONFIG_APP_WASM3_MVP_MODULE_MAX_SIZE", "RGBX_V2_MODULE_MAX_BYTES"],
];

const problems = [];

function check(condition, message) {
  if (!condition) problems.push(message);
}

// Key order is a JSON serializer's choice, not part of the profile.
function canonical(value) {
  return JSON.stringify(Object.entries(value).sort(([a], [b]) => (a < b ? -1 : 1)));
}

// Evaluate the ABI constants with the same compiler-backed extractor the
// packager uses. Reading the header with a regex is what let a decoy #define in
// an #if 0 block agree with a manifest while the firmware compiled a different
// value; the preprocessor reports only what the firmware actually compiles.
function abiConstants(root) {
  const script = join(root, "fw/sdk/tools/dump-abi-macros.sh");
  const includeDir = join(root, "fw/include");
  const run = spawnSync("bash", [script, includeDir], { encoding: "utf8" });
  if (run.status !== 0) {
    console.error(run.stderr || "dump-abi-macros.sh failed");
    process.exit(1);
  }
  const out = { abi: {}, cap: {}, policy: {} };
  for (const line of run.stdout.split("\n")) {
    if (!line) continue;
    const [kind, key, value] = line.split(" ");
    if (!(kind in out)) {
      console.error(`error: unexpected dump-abi-macros line: ${line}`);
      process.exit(1);
    }
    out[kind][key] = Number.parseInt(value, 10);
  }
  return out;
}

const [repoArg, manifestArg] = process.argv.slice(2);
if (!repoArg || !manifestArg) {
  console.error("usage: check-policy-sync.mjs <repo-root> <sdk-manifest.json>");
  process.exit(2);
}
const repoRoot = resolve(repoArg);
const headerPath = join(repoRoot, "fw/include/rgbx/rgbx_v2.h");
const headerBytes = readFileSync(headerPath);
const firmware = readFileSync(join(repoRoot, "fw/src/extensions/wasm_mvp_runtime.cpp"), "utf8");
const manifest = JSON.parse(readFileSync(resolve(manifestArg), "utf8"));
const policy = manifest.rgbxV2ModulePolicy ?? {};
const abi = abiConstants(repoRoot);

for (const field of Object.keys(POLICY_MACROS)) {
  const expected = abi.policy[field];
  check(Number.isInteger(expected), `dump-abi-macros produced no value for ${field}`);
  check(policy[field] === expected,
        `sdk-manifest rgbxV2ModulePolicy.${field} is ${policy[field]}, the ABI header is ${expected}`);
}

check(Object.keys(abi.cap).length > 0,
      "the ABI header declares no RGBX_V2_CAPABILITY_* permission bits");
check(canonical(policy.capabilityBits ?? {}) === canonical(abi.cap),
      "sdk-manifest capabilityBits do not match the ABI header's permission bits");

check(manifest.rgbxV2AbiVersion === abi.abi.rgbxV2AbiVersion,
      "sdk-manifest rgbxV2AbiVersion does not match the ABI header");
// abiVersion must be present, an integer, and match the header. A missing
// field vanishes silently in a `value > undefined` comparison (which is false),
// which would let the packager seal any non-negative integer.
check(Number.isInteger(manifest.abiVersion) && manifest.abiVersion === abi.abi.abiVersion,
      "sdk-manifest abiVersion is missing or does not match the ABI header");

// Structural sanity: some limits are not free tunables, they are fixed by the
// v2 import vocabulary. A module imports param_u32 (required) plus at most
// input_u32, set_good_moment and debug_u32, plus exactly one span encoding.
// So the fewest imports is 2 (param + span) and the most is 5. A consistent
// edit that raises the ceiling (5 -> 99) keeps the header, manifest and
// firmware agreeing, so only this structural check catches it.
const EXPECTED_MIN_IMPORTS = 2;
const EXPECTED_MAX_IMPORTS = 5;
check(abi.policy.minImports === EXPECTED_MIN_IMPORTS,
      `RGBX_V2_MIN_IMPORTS is ${abi.policy.minImports}; the import vocabulary fixes it at ${EXPECTED_MIN_IMPORTS}`);
check(abi.policy.maxImports === EXPECTED_MAX_IMPORTS,
      `RGBX_V2_MAX_IMPORTS is ${abi.policy.maxImports}; the import vocabulary fixes it at ${EXPECTED_MAX_IMPORTS}`);
// A tick may read each numeric parameter slot at most once, mirroring the
// firmware static_assert.
check(abi.policy.maxParamCallsPerTick === abi.policy.maxParams,
      "RGBX_V2_MAX_PARAM_CALLS_PER_TICK must equal RGBX_V2_MAX_PARAMS");
// A required section must be an admitted section.
check((abi.policy.sectionRequiredMask & ~abi.policy.sectionAllowedMask) === 0,
      "a required section id is not in the admitted section set");

// The manifest must describe the same header that produced these numbers,
// otherwise a stale archive could carry a profile no source in the tree
// declares any more.
const shippedDigest = (manifest.sdkFiles ?? {})["include/rgbx/rgbx_v2.h"];
check(shippedDigest === createHash("sha256").update(headerBytes).digest("hex"),
      "the SDK archive ships a different rgbx_v2.h than the one this profile was read from");

// Toolchain provenance: the version and the per-host archive digests the
// manifest publishes must be the ones the installers verify before use.
function shellPin(source, name, where) {
  const match = source.match(new RegExp(`^${name}="([0-9A-Za-z._-]+)"\\s*$`, "m"));
  if (match === null) {
    problems.push(`${name} is not a single-line pin in ${where}`);
    return null;
  }
  return match[1];
}

function checkToolchain({ installerPath, versionPin, digestPins, version, digests, label }) {
  const installer = readFileSync(join(repoRoot, installerPath), "utf8");
  check(shellPin(installer, versionPin, installerPath) === version,
        `sdk-manifest ${label} version does not match ${versionPin} in ${installerPath}`);
  const published = digests ?? {};
  for (const [host, variable] of Object.entries(digestPins)) {
    check(shellPin(installer, variable, installerPath) === published[host],
          `sdk-manifest ${label} digest for ${host} does not match ${variable}`);
  }
  check(Object.keys(published).length === Object.keys(digestPins).length,
        `sdk-manifest ${label} publishes digests for hosts the installer does not pin`);
}

checkToolchain({
  installerPath: "fw/sim/scripts/install-toolchain.sh",
  versionPin: "WASI_SDK_VERSION",
  digestPins: {
    "x86_64-linux": "WASI_SDK_SHA256_X86_64_LINUX",
    "arm64-linux": "WASI_SDK_SHA256_ARM64_LINUX",
    "x86_64-macos": "WASI_SDK_SHA256_X86_64_MACOS",
    "arm64-macos": "WASI_SDK_SHA256_ARM64_MACOS",
  },
  version: manifest.wasiSdk,
  digests: manifest.wasiSdkSha256,
  label: "wasi-sdk",
});

// The manifest spells the Arm toolchain as arm-gnu-<version>, matching the
// cache directory the installer creates.
const ARM_PREFIX = "arm-gnu-";
check(typeof manifest.armToolchain === "string" && manifest.armToolchain.startsWith(ARM_PREFIX),
      `sdk-manifest armToolchain must start with ${ARM_PREFIX}`);
checkToolchain({
  installerPath: "fw/sdk/scripts/install-arm-toolchain.sh",
  versionPin: "ARM_TOOLCHAIN_VERSION",
  digestPins: {
    "x86_64-linux": "ARM_TOOLCHAIN_SHA256_X86_64_LINUX",
    "aarch64-linux": "ARM_TOOLCHAIN_SHA256_AARCH64_LINUX",
    "x86_64-macos": "ARM_TOOLCHAIN_SHA256_DARWIN_X86_64",
    "arm64-macos": "ARM_TOOLCHAIN_SHA256_DARWIN_ARM64",
  },
  version: String(manifest.armToolchain).slice(ARM_PREFIX.length),
  digests: manifest.armToolchainSha256,
  label: "arm-gnu-toolchain",
});

for (const [constant, macro] of Object.entries(FIRMWARE_BINDINGS)) {
  check(new RegExp(`^constexpr\\s+\\w+\\s+${constant}\\s*=\\s*${macro};`, "m").test(firmware),
        `firmware constant ${constant} must be defined as ${macro}`);
}
for (const [constant, macro] of FIRMWARE_ASSERTIONS) {
  check(new RegExp(`(static_assert|BUILD_ASSERT)\\(\\s*${constant}\\s*==\\s*${macro}`, "m")
          .test(firmware),
        `firmware must assert ${constant} == ${macro}`);
}

// Manifest-format limits are not in the ABI header; they are the container
// format's own bounds, and the device parser (rgbx_package.h) and the host
// package builder (package-rgbx.mjs) each spell them out. They must agree, so
// cross-check them here. The parameter/string counts and the module ceiling
// are already tied to the ABI header by static_assert in rgbx_package.cpp.
const deviceHeader = readFileSync(join(repoRoot, "fw/src/extensions/rgbx_package.h"), "utf8");
const hostPackager = readFileSync(join(repoRoot, "fw/sdk/wasm-v2/package-rgbx.mjs"), "utf8");

function deviceConst(name) {
  const matches = [...deviceHeader.matchAll(
    new RegExp(`inline constexpr \\w[\\w ]*\\b${name}\\s*=\\s*([0-9]+)\\s*;`, "g"))];
  if (matches.length !== 1) {
    problems.push(`rgbx_package.h must define ${name} exactly once (found ${matches.length})`);
    return null;
  }
  return Number.parseInt(matches[0][1], 10);
}

function hostConst(name) {
  const matches = [...hostPackager.matchAll(
    new RegExp(`const ${name}\\s*=\\s*([0-9]+)\\s*;`, "g"))];
  if (matches.length !== 1) {
    problems.push(`package-rgbx.mjs must define ${name} exactly once (found ${matches.length})`);
    return null;
  }
  return Number.parseInt(matches[0][1], 10);
}

// device constant (rgbx_package.h) -> host constant (package-rgbx.mjs).
const MANIFEST_FORMAT_LIMITS = {
  kContainerVersion: "CONTAINER_VERSION",
  kHeaderSize: "HEADER_SIZE",
  kMaxManifestBytes: "MAX_MANIFEST_BYTES",
  kMaxExtensionIdLen: "MAX_EXTENSION_ID_LEN",
  kMaxDisplayNameLen: "MAX_DISPLAY_NAME_LEN",
  kMaxParamNameLen: "MAX_PARAM_NAME_LEN",
  kMaxSourceLanguageLen: "MAX_SOURCE_LANGUAGE_LEN",
  kMaxCompilerIdLen: "MAX_COMPILER_ID_LEN",
  kMaxCompilerVersionLen: "MAX_COMPILER_VERSION_LEN",
};
for (const [deviceName, hostName] of Object.entries(MANIFEST_FORMAT_LIMITS)) {
  const d = deviceConst(deviceName);
  const h = hostConst(hostName);
  check(d !== null && h !== null && d === h,
        `container-format limit drift: rgbx_package.h ${deviceName}=${d} but ` +
        `package-rgbx.mjs ${hostName}=${h}`);
}

if (problems.length > 0) {
  for (const problem of problems) console.error(`error: ${problem}`);
  process.exit(1);
}
console.log(`RGBX v2 policy is in sync across the ABI header, the SDK manifest, and the firmware ` +
            `(${Object.keys(POLICY_MACROS).length} pinned limits)`);
