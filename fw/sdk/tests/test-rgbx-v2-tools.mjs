#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
//
// Negative suite for the RGBX v2 SDK tools, plus a package-integrity check on
// the real container the SDK just produced.
//
// Two kinds of fixture appear here on purpose. Mutations of the compiled
// module prove the gate rejects damage to output a real compiler produced;
// hand-assembled modules prove the gate rejects shapes a compiler will not
// emit but an attacker can, and let one dimension be varied at a time. Every
// hand-assembled fixture is validated against the accept case first, so a
// rejection can never be an artifact of a broken builder.
//
//   test-rgbx-v2-tools.mjs <sdk-dir> <module.wasm> <package.rgbx>
//                          <manifest.json> <source-file>

import { createHash } from "node:crypto";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { spawnSync } from "node:child_process";

function fail(message) {
  throw new Error(message);
}

function readU32(bytes, state) {
  let value = 0;
  let shift = 0;
  for (;;) {
    if (state.offset >= bytes.length || shift >= 35) fail("malformed unsigned LEB128");
    const byte = bytes[state.offset++];
    value |= (byte & 0x7f) << shift;
    if ((byte & 0x80) === 0) return value >>> 0;
    shift += 7;
  }
}

function encodeU32(value) {
  const output = [];
  do {
    let byte = value & 0x7f;
    value >>>= 7;
    if (value !== 0) byte |= 0x80;
    output.push(byte);
  } while (value !== 0);
  return Buffer.from(output);
}

function insertSection(bytes, id, payload) {
  let cursor = 8;
  while (cursor < bytes.length) {
    const sectionStart = cursor;
    const sectionId = bytes[cursor++];
    const state = { offset: cursor };
    const size = readU32(bytes, state);
    cursor = state.offset + size;
    if (sectionId !== 0 && sectionId > id) {
      const section = Buffer.concat([Buffer.from([id]), encodeU32(payload.length), payload]);
      return Buffer.concat([bytes.subarray(0, sectionStart), section, bytes.subarray(sectionStart)]);
    }
  }
  return Buffer.concat([bytes, Buffer.from([id]), encodeU32(payload.length), payload]);
}

// ---------------------------------------------------------------------------
// Minimal WebAssembly assembler. It exists so one admission dimension can be
// varied at a time; it is not a general encoder and only emits what the
// memoryless profile allows.
// ---------------------------------------------------------------------------

const I32 = 0x7f;

function vec(items) {
  return Buffer.concat([encodeU32(items.length), ...items]);
}

function section(id, payload) {
  return Buffer.concat([Buffer.from([id]), encodeU32(payload.length), payload]);
}

function wasmName(text) {
  const bytes = Buffer.from(text, "ascii");
  return Buffer.concat([encodeU32(bytes.length), bytes]);
}

function functionType(params, results) {
  return Buffer.concat([
    Buffer.from([0x60]),
    vec(params.map((type) => Buffer.from([type]))),
    vec(results.map((type) => Buffer.from([type]))),
  ]);
}

function functionBody(localGroups, code) {
  const body = Buffer.concat([vec(localGroups), code]);
  return Buffer.concat([encodeU32(body.length), body]);
}

// i32.const takes a SIGNED LEB128 immediate. The unsigned form is not
// interchangeable: 64 encodes as a single 0x40 byte, whose sign bit is set, so
// an unsigned encoder would emit -64 for every pixel offset that lands on a
// 64-byte boundary.
function i32Const(value) {
  const output = [0x41];
  let remaining = value;
  for (;;) {
    const byte = remaining & 0x7f;
    remaining >>= 7;
    const signBit = (byte & 0x40) !== 0;
    if ((remaining === 0 && !signBit) || (remaining === -1 && signBit)) {
      output.push(byte);
      return Buffer.from(output);
    }
    output.push(byte | 0x80);
  }
}

// Straight-line code emitting `count` consecutive full spans through
// set_span8, which the assembled fixtures always import at function index 1.
function spanCalls(count, pixelsPerSpan, setSpanIndex) {
  const parts = [];
  for (let index = 0; index < count; ++index) {
    parts.push(i32Const(index * pixelsPerSpan));
    for (let pixel = 0; pixel < pixelsPerSpan; ++pixel) parts.push(i32Const(0));
    parts.push(Buffer.from([0x10]), encodeU32(setSpanIndex));
  }
  parts.push(Buffer.from([0x0b]));
  return Buffer.concat(parts);
}

const IMPORT_SIGNATURES = {
  param_u32: [[I32], [I32]],
  input_u32: [[I32, I32], [I32]],
  set_good_moment: [[I32], []],
  debug_u32: [[I32, I32], []],
  set_span8: [null, []],
  set_luma_span8: [null, []],
};

/**
 * Assemble a module with exactly the requested shape.
 *
 * `imports` names the host imports in order, `globals` the count of defined
 * i32 globals, `tickLocals` the locals declared by rgbx_tick, and `tickCode`
 * its body (defaulting to a complete frame).
 */
function assembleModule(policy, options = {}) {
  const imports = options.imports ?? ["param_u32", "set_span8"];
  const globals = options.globals ?? 0;
  const tickLocals = options.tickLocals ?? 0;
  const span = policy.pixelsPerSpan;

  const types = [];
  const importEntries = [];
  for (const name of imports) {
    const [params, results] = IMPORT_SIGNATURES[name];
    const actualParams = params ??
      Array(name === "set_luma_span8" ? span + 3 : span + 1).fill(I32);
    types.push(functionType(actualParams, results));
    importEntries.push(Buffer.concat([
      wasmName("rgbx_v2"), wasmName(name), Buffer.from([0x00]),
      encodeU32(types.length - 1),
    ]));
  }
  const initTypeIndex = types.length;
  types.push(functionType([], []));
  const tickTypeIndex = types.length;
  types.push(functionType([I32], []));

  const initIndex = imports.length;
  const tickIndex = imports.length + 1;
  const setSpanIndex = imports.indexOf("set_span8");
  const tickCode = options.tickCode ??
    spanCalls(policy.spanCallsPerTick, span, setSpanIndex);

  const parts = [
    Buffer.from([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0]),
    section(1, vec(types)),
    section(2, vec(importEntries)),
    section(3, vec([encodeU32(initTypeIndex), encodeU32(tickTypeIndex)])),
  ];
  if (globals > 0) {
    parts.push(section(6, vec(Array.from({ length: globals }, () =>
      Buffer.concat([Buffer.from([I32, 0x00]), i32Const(0), Buffer.from([0x0b])])))));
  }
  parts.push(
    section(7, vec([
      Buffer.concat([wasmName("rgbx_init"), Buffer.from([0x00]), encodeU32(initIndex)]),
      Buffer.concat([wasmName("rgbx_tick"), Buffer.from([0x00]), encodeU32(tickIndex)]),
    ])),
    section(10, vec([
      functionBody([], Buffer.from([0x0b])),
      functionBody(tickLocals > 0 ? [Buffer.concat([encodeU32(tickLocals), Buffer.from([I32])])] : [],
                   tickCode),
    ])),
  );
  return Buffer.concat(parts);
}

// ---------------------------------------------------------------------------

const [sdkArg, wasmArg, packageArg, specArg, sourceArg] = process.argv.slice(2);
if (!sdkArg || !wasmArg || !packageArg || !specArg || !sourceArg) {
  console.error("usage: test-rgbx-v2-tools.mjs <sdk-dir> <module.wasm> <package.rgbx> " +
                "<manifest.json> <source-file>");
  process.exit(2);
}

const sdk = resolve(sdkArg);
const wasm = readFileSync(resolve(wasmArg));
const packageBytes = readFileSync(resolve(packageArg));
const sourceFile = resolve(sourceArg);
const checker = join(sdk, "wasm-v2/check-rgbx-v2.mjs");
const prepare = join(sdk, "wasm-v2/prepare-rgbx-v2.mjs");
const packager = join(sdk, "wasm-v2/package-rgbx.mjs");
const sdkManifestPath = join(sdk, "sdk-manifest.json");
const sdkManifest = JSON.parse(readFileSync(sdkManifestPath, "utf8"));
const pinned = sdkManifest.rgbxV2ModulePolicy;
if (!pinned) fail("the packaged SDK manifest carries no rgbxV2ModulePolicy");
const policy = {
  ...pinned,
  pixelCount: pinned.width * pinned.height,
  spanCallsPerTick: (pinned.width * pinned.height) / pinned.pixelsPerSpan,
};
const baseSpec = JSON.parse(readFileSync(resolve(specArg), "utf8"));

const scratch = mkdtempSync(join(tmpdir(), "rgbx-v2-tools-"));
let checked = 0;

function runChecker(bytes, label) {
  const file = join(scratch, `${label.replaceAll(" ", "-")}.wasm`);
  writeFileSync(file, bytes);
  return spawnSync(process.execPath, [checker, file], {
    encoding: "utf8",
    env: { ...process.env, RGBX_SDK_MANIFEST: sdkManifestPath },
  });
}

function expectModuleAccepted(label, bytes) {
  const result = runChecker(bytes, label);
  if (result.status !== 0) {
    fail(`${label} should have been admitted: ${result.stderr || result.stdout}`);
  }
  ++checked;
}

function expectModuleRejected(label, bytes) {
  const result = runChecker(bytes, label);
  if (result.status === 0) fail(`${label} unexpectedly passed the module gate`);
  ++checked;
}

function expectPackagerRejected(label, spec, moduleBytes = wasm, manifestPath = sdkManifestPath) {
  const specPath = join(scratch, `${label.replaceAll(" ", "-")}.json`);
  const modulePath = join(scratch, `${label.replaceAll(" ", "-")}.module.wasm`);
  writeFileSync(specPath, JSON.stringify(spec));
  writeFileSync(modulePath, moduleBytes);
  const result = spawnSync(
    process.execPath,
    [packager, specPath, modulePath, join(scratch, "should-not-exist.rgbx")],
    {
      encoding: "utf8",
      env: {
        ...process.env,
        RGBX_SDK_MANIFEST: manifestPath,
        RGBX_SOURCE_FILE: sourceFile,
      },
    },
  );
  if (result.status === 0) fail(`${label} unexpectedly produced a package`);
  ++checked;
}

// A spec whose sourceFile is absolute, so mutated copies can live in scratch
// and still name the translation unit CMake compiled.
function specWith(overrides) {
  const spec = { ...baseSpec, sourceFile: sourceFile, ...overrides };
  for (const [key, value] of Object.entries(overrides)) {
    if (value === undefined) delete spec[key];
  }
  return spec;
}

try {
  // --- Post-link gate: damage to real compiler output ---------------------
  expectModuleRejected("custom section", insertSection(wasm, 0, Buffer.from([0])));
  expectModuleRejected("memory section", insertSection(wasm, 5, Buffer.from([1, 0, 1])));

  let exportSectionStart = 8;
  let exportSectionEnd = 0;
  while (exportSectionStart < wasm.length) {
    const id = wasm[exportSectionStart];
    const state = { offset: exportSectionStart + 1 };
    const size = readU32(wasm, state);
    const end = state.offset + size;
    if (id === 7) {
      exportSectionEnd = end;
      expectModuleRejected("duplicate export section",
                           Buffer.concat([wasm.subarray(0, end),
                                          wasm.subarray(exportSectionStart, end),
                                          wasm.subarray(end)]));
      break;
    }
    exportSectionStart = end;
  }
  if (!exportSectionEnd) fail("fixture has no export section");

  const renamedImport = Buffer.from(wasm);
  const importName = renamedImport.indexOf(Buffer.from("param_u32", "ascii"));
  if (importName < 0) fail("fixture has no param_u32 import");
  renamedImport[importName] = "x".charCodeAt(0);
  expectModuleRejected("unknown import", renamedImport);

  const floatOpcode = Buffer.from(wasm);
  let codeCursor = 8;
  let replacedIntegerConst = false;
  while (codeCursor < floatOpcode.length) {
    const id = floatOpcode[codeCursor++];
    const state = { offset: codeCursor };
    const size = readU32(floatOpcode, state);
    const end = state.offset + size;
    if (id === 10) {
      const opcode = floatOpcode.indexOf(0x41, state.offset);
      if (opcode < state.offset || opcode >= end) fail("fixture code has no i32.const opcode");
      floatOpcode[opcode] = 0x43; // f32.const must be rejected by the integer-only profile.
      replacedIntegerConst = true;
      break;
    }
    codeCursor = end;
  }
  if (!replacedIntegerConst) fail("fixture has no code section");
  expectModuleRejected("floating-point opcode", floatOpcode);

  // The module ceiling is the device's fixed buffer, so a module one byte over
  // it must never reach a package. Padding a section keeps the module
  // structurally plausible right up to the size check.
  expectModuleRejected(
    "module over the size ceiling",
    insertSection(wasm, 6, Buffer.alloc(policy.moduleMaxBytes, 0)));

  const withStart = insertSection(wasm, 8, Buffer.from([2]));
  const startFile = join(scratch, "start.wasm");
  writeFileSync(startFile, withStart);
  if (spawnSync(process.execPath, [prepare, startFile, join(scratch, "out.wasm")],
                { encoding: "utf8" }).status === 0) {
    fail("post-link normalization accepted a start function");
  }
  ++checked;

  // --- Post-link gate: one admission dimension at a time ------------------
  // The accept case first: everything below differs from it in exactly one
  // way, so a rejection cannot be blamed on the assembler.
  expectModuleAccepted("assembled reference module", assembleModule(policy));

  expectModuleRejected("too few imports", assembleModule(policy, { imports: ["param_u32"] }));
  expectModuleRejected("no span encoding",
                       assembleModule(policy, { imports: ["param_u32", "input_u32"] }));
  expectModuleRejected(
    "both span encodings",
    assembleModule(policy, {
      imports: ["param_u32", "set_span8", "set_luma_span8"],
      tickCode: spanCalls(policy.spanCallsPerTick, policy.pixelsPerSpan, 1),
    }));
  expectModuleRejected(
    "more imports than the profile admits",
    assembleModule(policy, {
      imports: ["param_u32", "input_u32", "set_good_moment", "debug_u32", "set_span8",
                "set_luma_span8"],
    }));
  expectModuleRejected("too many globals",
                       assembleModule(policy, { globals: policy.maxGlobals + 1 }));
  expectModuleRejected("too many locals",
                       assembleModule(policy, { tickLocals: policy.maxLocalsPerFunction + 1 }));
  expectModuleRejected(
    "incomplete frame",
    assembleModule(policy, {
      tickCode: spanCalls(policy.spanCallsPerTick - 1, policy.pixelsPerSpan, 1),
    }));
  expectModuleRejected(
    "more spans than one frame",
    assembleModule(policy, {
      tickCode: spanCalls(policy.spanCallsPerTick + 1, policy.pixelsPerSpan, 1),
    }));

  // A guest that never returns must be bounded by the oracle's deadline
  // rather than hanging the build. `loop (void) ; br 0 ; end` is the smallest
  // module that passes every structural check and then does not terminate.
  const started = Date.now();
  expectModuleRejected(
    "nonterminating guest",
    assembleModule(policy, { tickCode: Buffer.from([0x03, 0x40, 0x0c, 0x00, 0x0b, 0x0b]) }));
  const elapsed = Date.now() - started;
  if (elapsed > 30000) fail(`the oracle took ${elapsed} ms to bound a nonterminating guest`);

  // --- Package builder: every field the firmware later trusts -------------
  expectPackagerRejected("wrong compiler pin", specWith({ compilerVersion: "0.0" }));
  expectPackagerRejected("wrong ABI pin", specWith({ rgbxAbi: baseSpec.rgbxAbi + 1 }));
  expectPackagerRejected("source that was not compiled",
                         specWith({ sourceFile: join(scratch, "not-the-compiled-source.c") }));
  expectPackagerRejected("hand-supplied source digest",
                         specWith({ sourceDigest: "0".repeat(64) }));
  expectPackagerRejected("nonzero linear memory", specWith({ memoryMaxBytes: 1 }));
  expectPackagerRejected("unsupported budget class", specWith({ budgetClass: 1 }));
  expectPackagerRejected("unrecognized manifest key", specWith({ trustMe: true }));
  expectPackagerRejected("geometry from another display",
                         specWith({ geometry: [policy.width, policy.height + 1] }));
  expectPackagerRejected("firmware ABI from the future",
                         specWith({ minimumFirmwareAbi: sdkManifest.abiVersion + 1 }));
  // A manifest missing abiVersion once let the packager seal any
  // minimumFirmwareAbi (value > undefined is false). It must now be rejected.
  const noAbiManifestPath = join(scratch, "sdk-manifest-no-abi.json");
  const noAbi = { ...sdkManifest };
  delete noAbi.abiVersion;
  writeFileSync(noAbiManifestPath, JSON.stringify(noAbi));
  expectPackagerRejected("sdk manifest missing abiVersion", specWith({}), wasm, noAbiManifestPath);
  expectPackagerRejected("semantic version outside uint16",
                         specWith({ version: [0x10000, 0, 0] }));
  expectPackagerRejected("unknown capability", specWith({ capabilities: ["root"] }));
  expectPackagerRejected(
    "more parameters than the profile admits",
    specWith({
      parameters: Array.from({ length: policy.maxParams + 1 }, (unused, index) =>
        ({ name: `P${index}`, type: "uint32", default: 0 })),
    }));
  expectPackagerRejected(
    "more string parameters than the profile admits",
    specWith({
      parameters: Array.from({ length: policy.maxStringParams + 1 }, (unused, index) =>
        ({ name: `S${index}`, type: "string", default: "" })),
    }));
  expectPackagerRejected(
    "string default longer than the device slot",
    specWith({
      parameters: [{ name: "Label", type: "string", default: "x".repeat(policy.stringParamSize) }],
    }));
  expectPackagerRejected("module over the size ceiling",
                         specWith({}), Buffer.alloc(policy.moduleMaxBytes + 1));

  // --- The container the SDK actually produced ----------------------------
  if (packageBytes.subarray(0, 4).toString("ascii") !== "RGBX" ||
      packageBytes.readUInt16LE(4) !== 1 || packageBytes.readUInt16LE(6) !== 20 ||
      packageBytes.readUInt32LE(16) !== 0) {
    fail("package header is not canonical RGBX v1");
  }
  const manifestLength = packageBytes.readUInt32LE(8);
  const wasmLength = packageBytes.readUInt32LE(12);
  const coveredLength = 20 + manifestLength + wasmLength;
  if (coveredLength + 32 !== packageBytes.length ||
      !packageBytes.subarray(20 + manifestLength, coveredLength).equals(wasm) ||
      !createHash("sha256").update(packageBytes.subarray(0, coveredLength)).digest()
        .equals(packageBytes.subarray(coveredLength))) {
    fail("package length, module payload, or digest is inconsistent");
  }
  console.log(`RGBX v2 SDK tool tests passed: ${checked} rejections and package integrity`);
} finally {
  rmSync(scratch, { recursive: true, force: true });
}
