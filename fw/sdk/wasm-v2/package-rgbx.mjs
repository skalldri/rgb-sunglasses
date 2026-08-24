#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Stuart Alldritt
//
// Build the canonical RGBX container: a fixed 20-byte header, a canonical
// CBOR manifest, the gated Wasm module, and a SHA-256 trailer over all three.
//
// Everything the firmware later reads out of the manifest is checked here
// first, against the release's pinned admission profile rather than against
// constants of this script's own. Two rules keep that honest:
//
//   * no field reaches the encoder unvalidated: a value the device would
//     refuse must be refused here, where the author still sees why;
//   * no key in the author's JSON is ignored: an unrecognized key is an
//     error, so a typo in a security-relevant field cannot silently fall back
//     to a permissive default.

import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";

import { loadSdkManifest, modulePolicy } from "./rgbx-v2-policy.mjs";

// Bounds the on-device parser applies to the manifest's text and table fields.
// They are the container format's own limits, fixed by the RGBX v1 container
// version this builder emits, not per-release tuning.
const CONTAINER_VERSION = 1;
const HEADER_SIZE = 20;
const MANIFEST_VERSION = 1;
const MAX_MANIFEST_BYTES = 2048;
const MAX_EXTENSION_ID_LEN = 31;
const MAX_DISPLAY_NAME_LEN = 31;
const MAX_PARAM_NAME_LEN = 19;
const MAX_SOURCE_LANGUAGE_LEN = 15;
const MAX_COMPILER_ID_LEN = 31;
const MAX_COMPILER_VERSION_LEN = 15;
const SEMANTIC_VERSION_MAX = 0xffff;
const WASM_MAGIC = Buffer.from([0, 97, 115, 109, 1, 0, 0, 0]);

const PARAM_TYPES = { uint32: 0, color: 1, bool: 2, string: 3 };
const PARAM_KEYS = new Set(["name", "type", "default"]);
const SPEC_KEYS = new Set([
  "extensionId", "displayName", "version", "rgbxAbi", "minimumFirmwareAbi", "geometry",
  "capabilities", "memoryMaxBytes", "budgetClass", "sourceLanguage", "compilerId",
  "compilerVersion", "sourceFile", "parameters",
]);

function fail(message) {
  throw new Error(message);
}

function typeAndLength(major, value) {
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) {
    fail(`CBOR length/value is outside uint32: ${value}`);
  }
  if (value < 24) return Buffer.from([(major << 5) | value]);
  if (value <= 0xff) return Buffer.from([(major << 5) | 24, value]);
  if (value <= 0xffff) {
    const output = Buffer.alloc(3);
    output[0] = (major << 5) | 25;
    output.writeUInt16BE(value, 1);
    return output;
  }
  const output = Buffer.alloc(5);
  output[0] = (major << 5) | 26;
  output.writeUInt32BE(value, 1);
  return output;
}

function cborUint(value) {
  return typeAndLength(0, value);
}

function cborBool(value) {
  return Buffer.from([value ? 0xf5 : 0xf4]);
}

function cborText(value) {
  const bytes = Buffer.from(value, "ascii");
  if (bytes.toString("ascii") !== value || /[^\x20-\x7e]/.test(value)) {
    fail(`manifest text must be printable ASCII: ${JSON.stringify(value)}`);
  }
  return Buffer.concat([typeAndLength(3, bytes.length), bytes]);
}

function cborBytes(value) {
  return Buffer.concat([typeAndLength(2, value.length), value]);
}

function cborArray(items) {
  return Buffer.concat([typeAndLength(4, items.length), ...items]);
}

function rejectUnknownKeys(value, allowed, where) {
  for (const key of Object.keys(value)) {
    if (!allowed.has(key)) fail(`${where} carries unrecognized key ${JSON.stringify(key)}`);
  }
}

function boundedText(value, name, maxLength) {
  if (typeof value !== "string" || value.length === 0 || value.length > maxLength) {
    fail(`${name} must contain 1..${maxLength} ASCII bytes`);
  }
  return cborText(value);
}

function requireUint(value, name, maximum) {
  if (!Number.isInteger(value) || value < 0 || value > maximum) {
    fail(`${name} must be an integer in 0..${maximum}`);
  }
  return value;
}

function encodeCapabilities(spec, policy) {
  const requested = spec.capabilities ?? [];
  if (!Array.isArray(requested)) fail("capabilities must be an array of capability names");
  let bits = 0;
  const seen = new Set();
  for (const capability of requested) {
    const bit = policy.capabilityBits[capability];
    if (bit === undefined) {
      fail(`unknown capability: ${JSON.stringify(capability)}`);
    }
    if (seen.has(capability)) fail(`capability ${capability} is listed twice`);
    seen.add(capability);
    bits |= bit;
  }
  return bits;
}

// The device records this digest as the package's provenance. There is no way
// for a manifest to supply one directly: the only digest worth recording is
// the one this builder computed over the exact translation unit the build
// compiled, which the caller has already been made to name.
function encodeSourceDigest(spec, specPath) {
  if (typeof spec.sourceFile !== "string" || spec.sourceFile.length === 0) {
    fail("sourceFile must be a path relative to the manifest");
  }
  return createHash("sha256")
    .update(readFileSync(resolve(dirname(specPath), spec.sourceFile)))
    .digest();
}

function encodeParams(spec, policy) {
  const params = spec.parameters ?? [];
  if (!Array.isArray(params) || params.length > policy.maxParams) {
    fail(`this runtime admits at most ${policy.maxParams} parameters per RGBX package`);
  }
  let stringParams = 0;
  const names = new Set();
  return params.map((param) => {
    if (param === null || typeof param !== "object" || Array.isArray(param)) {
      fail("each parameter must be an object");
    }
    rejectUnknownKeys(param, PARAM_KEYS, `parameter ${JSON.stringify(param.name)}`);
    if (!(param.type in PARAM_TYPES)) fail(`unknown parameter type: ${param.type}`);
    const name = boundedText(param.name, "parameter name", MAX_PARAM_NAME_LEN);
    if (names.has(param.name)) fail(`parameter ${param.name} is declared twice`);
    names.add(param.name);

    let encodedDefault;
    if (param.type === "string") {
      if (++stringParams > policy.maxStringParams) {
        fail(`at most ${policy.maxStringParams} string parameters are allowed`);
      }
      // One byte of the device's slot is the NUL terminator.
      const maxLength = policy.stringParamSize - 1;
      if (typeof param.default !== "string" || param.default.length > maxLength) {
        fail(`${param.name} string default must contain at most ${maxLength} ASCII bytes`);
      }
      encodedDefault = cborText(param.default);
    } else if (param.type === "bool") {
      if (typeof param.default !== "boolean") fail(`${param.name} default must be boolean`);
      encodedDefault = cborBool(param.default);
    } else {
      requireUint(param.default, `${param.name} default`, 0xffffffff);
      if (param.type === "color" && param.default > 0xffffff) {
        fail(`${param.name} color default must be 0x000000..0xffffff`);
      }
      encodedDefault = cborUint(param.default);
    }
    return cborArray([cborUint(PARAM_TYPES[param.type]), name, encodedDefault]);
  });
}

function encodeManifest(spec, specPath, sdk, policy) {
  rejectUnknownKeys(spec, SPEC_KEYS, "manifest");

  if (!/^[a-z0-9][a-z0-9._-]{0,30}$/.test(spec.extensionId ?? "")) {
    fail("extensionId must match ^[a-z0-9][a-z0-9._-]{0,30}$");
  }
  if (spec.extensionId.length > MAX_EXTENSION_ID_LEN) {
    fail(`extensionId must contain at most ${MAX_EXTENSION_ID_LEN} ASCII bytes`);
  }
  if (!Array.isArray(spec.version) || spec.version.length !== 3) {
    fail("version must be [major, minor, patch]");
  }
  spec.version.forEach((part, index) =>
    requireUint(part, `version component ${index}`, SEMANTIC_VERSION_MAX));

  // Geometry, ABI, and compiler identity are the fields the device trusts to
  // decide whether a package belongs on this hardware at all. Each is pinned
  // to the release, not merely range-checked.
  if (!Array.isArray(spec.geometry) || spec.geometry.length !== 2 ||
      spec.geometry[0] !== policy.width || spec.geometry[1] !== policy.height) {
    fail(`geometry must be [${policy.width}, ${policy.height}] for this release`);
  }
  if (spec.rgbxAbi !== sdk.rgbxV2AbiVersion) {
    fail(`rgbxAbi ${spec.rgbxAbi} does not match SDK ABI ${sdk.rgbxV2AbiVersion}`);
  }
  requireUint(spec.minimumFirmwareAbi, "minimumFirmwareAbi", sdk.abiVersion);
  if (spec.compilerId !== "wasi-sdk" || spec.compilerVersion !== sdk.wasiSdk) {
    fail(`compiler ${spec.compilerId}/${spec.compilerVersion} does not match SDK ` +
         `wasi-sdk/${sdk.wasiSdk}`);
  }
  // The memoryless profile has no linear memory and one budget class; a
  // package that asks for either is rejected before it can reach a device
  // whose parser would have to refuse it at install time instead.
  if (spec.memoryMaxBytes !== 0) {
    fail("memoryMaxBytes must be 0: the RGBX v2 profile grants no linear memory");
  }
  if (spec.budgetClass !== 0) {
    fail("budgetClass must be 0: this release defines exactly one budget class");
  }

  return cborArray([
    cborUint(MANIFEST_VERSION),
    cborText(spec.extensionId),
    boundedText(spec.displayName, "displayName", MAX_DISPLAY_NAME_LEN),
    cborArray(spec.version.map(cborUint)),
    cborUint(spec.rgbxAbi),
    cborUint(spec.minimumFirmwareAbi),
    cborArray(spec.geometry.map(cborUint)),
    cborUint(encodeCapabilities(spec, policy)),
    cborUint(spec.memoryMaxBytes),
    cborUint(spec.budgetClass),
    boundedText(spec.sourceLanguage, "sourceLanguage", MAX_SOURCE_LANGUAGE_LEN),
    boundedText(spec.compilerId, "compilerId", MAX_COMPILER_ID_LEN),
    boundedText(spec.compilerVersion, "compilerVersion", MAX_COMPILER_VERSION_LEN),
    cborBytes(encodeSourceDigest(spec, specPath)),
    cborArray(encodeParams(spec, policy)),
  ]);
}

function usage() {
  console.error("Usage: package-rgbx.mjs <manifest.json> <module.wasm> <output.rgbx>");
  process.exit(2);
}

const [manifestArg, wasmArg, outputArg] = process.argv.slice(2);
if (!manifestArg || !wasmArg || !outputArg) usage();

const manifestPath = resolve(manifestArg);
const wasmPath = resolve(wasmArg);
const outputPath = resolve(outputArg);
const spec = JSON.parse(readFileSync(manifestPath, "utf8"));
if (spec === null || typeof spec !== "object" || Array.isArray(spec)) {
  fail("the extension manifest must be a JSON object");
}
// The recorded source digest is only provenance if it covers the translation
// unit CMake actually compiled into this package.
if (!process.env.RGBX_SOURCE_FILE ||
    resolve(dirname(manifestPath), spec.sourceFile ?? "") !== resolve(process.env.RGBX_SOURCE_FILE)) {
  fail("manifest sourceFile does not identify the translation unit compiled by CMake");
}
const sdk = loadSdkManifest();
const policy = modulePolicy(sdk);
const manifest = encodeManifest(spec, manifestPath, sdk, policy);
const wasm = readFileSync(wasmPath);
if (manifest.length === 0 || manifest.length > MAX_MANIFEST_BYTES) {
  fail(`manifest is ${manifest.length} bytes`);
}
// The device stores the module in a fixed buffer sized by the release's
// profile; anything larger is unloadable, so it must not be packaged.
if (wasm.length < WASM_MAGIC.length || wasm.length > policy.moduleMaxBytes) {
  fail(`Wasm module is ${wasm.length} bytes, outside ${WASM_MAGIC.length}..${policy.moduleMaxBytes}`);
}
if (!wasm.subarray(0, WASM_MAGIC.length).equals(WASM_MAGIC)) {
  fail("input is not a WebAssembly v1 module");
}

const header = Buffer.alloc(HEADER_SIZE);
header.write("RGBX", 0, "ascii");
header.writeUInt16LE(CONTAINER_VERSION, 4);
header.writeUInt16LE(HEADER_SIZE, 6);
header.writeUInt32LE(manifest.length, 8);
header.writeUInt32LE(wasm.length, 12);
header.writeUInt32LE(0, 16);
const covered = Buffer.concat([header, manifest, wasm]);
const digest = createHash("sha256").update(covered).digest();
const output = Buffer.concat([covered, digest]);
writeFileSync(outputPath, output);
console.log(
  `${spec.displayName}: ${output.length}-byte RGBX package, ${wasm.length}-byte Wasm, ` +
    `SHA-256 ${createHash("sha256").update(output).digest("hex")}`,
);
