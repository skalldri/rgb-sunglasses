#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Stuart Alldritt
//
// Post-link admission gate for a memoryless RGBX v2 module.
//
// Two independent passes, both of which must succeed:
//
//   1. A structural pass that re-derives the firmware's admission decision
//      from the module bytes. Every limit it applies comes from the SDK
//      manifest's pinned profile (rgbx-v2-policy.mjs), never from a constant
//      written down here, so this gate and the device cannot disagree about
//      what is admissible.
//   2. A tick oracle that actually runs rgbx_init and one rgbx_tick against a
//      host enforcing the same per-tick call budgets the firmware enforces,
//      proving the module paints a complete frame rather than only looking
//      like it could.
//
// The oracle executes an untrusted guest, so it never runs on this thread: it
// runs in a worker with a wall deadline and V8 resource limits, and anything
// that outlives or outgrows those bounds is a rejection, not a hang.

import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { isMainThread, parentPort, workerData, Worker } from "node:worker_threads";

import { loadSdkManifest, modulePolicy } from "./rgbx-v2-policy.mjs";

// Wall budget for one complete oracle run (compile, rgbx_init, one rgbx_tick).
// A conforming module finishes in single-digit milliseconds; this is sized so
// a loaded CI runner cannot produce a false rejection, while a guest that does
// not terminate is still bounded.
const ORACLE_DEADLINE_MS = 1000;

// V8 caps for the oracle worker. A memoryless module has no linear memory to
// grow, so these bound the interpreter and the host-side accounting rather
// than guest data, and they turn a pathological module into an out-of-memory
// rejection instead of host memory pressure.
const ORACLE_RESOURCE_LIMITS = {
  maxOldGenerationSizeMb: 64,
  maxYoungGenerationSizeMb: 16,
  codeRangeSizeMb: 32,
  stackSizeMb: 4,
};

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

function readName(bytes, state) {
  const length = readU32(bytes, state);
  if (length > bytes.length - state.offset) fail("name extends beyond its section");
  const value = bytes.subarray(state.offset, state.offset + length).toString("utf8");
  state.offset += length;
  return value;
}

function skipLeb(bytes, state, maxBytes) {
  for (let count = 0; count < maxBytes; ++count) {
    if (state.offset >= bytes.length) fail("truncated LEB128 immediate");
    if ((bytes[state.offset++] & 0x80) === 0) return;
  }
  fail("oversized LEB128 immediate");
}

function sections(bytes, policy) {
  const result = new Map();
  let cursor = 8;
  let lastSection = 0;
  while (cursor < bytes.length) {
    const id = bytes[cursor++];
    const sizeState = { offset: cursor };
    const size = readU32(bytes, sizeState);
    cursor = sizeState.offset;
    if (size > bytes.length - cursor) fail("section extends beyond the module");
    if (!policy.allowedSections.has(id) || id <= lastSection || result.has(id)) {
      fail(`unexpected, duplicate, or out-of-order section ${id}`);
    }
    lastSection = id;
    result.set(id, bytes.subarray(cursor, cursor + size));
    cursor += size;
  }
  for (const id of policy.requiredSections) {
    if (!result.has(id)) fail(`module omits required section ${id}`);
  }
  return result;
}

function parseTypes(payload, policy) {
  if (!payload) fail("missing type section");
  // The widest host import is the palette-luma span: a first-pixel index, a
  // foreground and a background color, then one luma per pixel in the span.
  const maxParams = policy.pixelsPerSpan + 3;
  const state = { offset: 0 };
  const count = readU32(payload, state);
  const types = [];
  for (let i = 0; i < count; ++i) {
    if (payload[state.offset++] !== 0x60) fail("unsupported non-function type");
    const paramCount = readU32(payload, state);
    const params = Array.from(payload.subarray(state.offset, state.offset + paramCount));
    state.offset += paramCount;
    const resultCount = readU32(payload, state);
    const results = Array.from(payload.subarray(state.offset, state.offset + resultCount));
    state.offset += resultCount;
    if (params.length !== paramCount || results.length !== resultCount) {
      fail("truncated function type");
    }
    if (paramCount > maxParams || resultCount > 1) {
      fail("function signature exceeds the v2 frame bound");
    }
    if ([...params, ...results].some((value) => value !== 0x7f && value !== 0x7e)) {
      fail("memoryless RGBX v2 types may use only i32 and i64");
    }
    types.push({ params, results });
  }
  if (state.offset !== payload.length) fail("trailing bytes in type section");
  return types;
}

function sameType(actual, params, results) {
  return actual && actual.params.join(",") === params.join(",") &&
         actual.results.join(",") === results.join(",");
}

function importSignatures(policy) {
  const span = policy.pixelsPerSpan;
  return new Map([
    ["param_u32", [[0x7f], [0x7f]]],
    ["input_u32", [[0x7f, 0x7f], [0x7f]]],
    ["set_good_moment", [[0x7f], []]],
    ["debug_u32", [[0x7f, 0x7f], []]],
    ["set_span8", [Array(span + 1).fill(0x7f), []]],
    ["set_luma_span8", [Array(span + 3).fill(0x7f), []]],
  ]);
}

function parseImports(payload, types, policy) {
  if (!payload) fail("missing import section");
  const expected = importSignatures(policy);
  const state = { offset: 0 };
  const count = readU32(payload, state);
  if (count < policy.minImports || count > policy.maxImports) {
    fail(`expected ${policy.minImports}..${policy.maxImports} function imports, found ${count}`);
  }
  const imports = [];
  const names = new Set();
  for (let i = 0; i < count; ++i) {
    const module = readName(payload, state);
    const name = readName(payload, state);
    if (payload[state.offset++] !== 0) fail("RGBX v2 may import functions only");
    const typeIndex = readU32(payload, state);
    const signature = expected.get(name);
    if (module !== "rgbx_v2" || !signature || names.has(name) ||
        !sameType(types[typeIndex], signature[0], signature[1])) {
      fail(`unexpected or wrongly typed import ${module}.${name}`);
    }
    names.add(name);
    imports.push({ name, typeIndex });
  }
  if (state.offset !== payload.length) fail("trailing bytes in import section");
  if (!names.has("param_u32") || (names.has("set_span8") === names.has("set_luma_span8"))) {
    fail("module needs param_u32 and exactly one span encoding");
  }
  return imports;
}

function parseFunctionTypes(payload) {
  if (!payload) fail("missing function section");
  const state = { offset: 0 };
  const count = readU32(payload, state);
  const indices = [];
  for (let i = 0; i < count; ++i) indices.push(readU32(payload, state));
  if (state.offset !== payload.length) fail("trailing bytes in function section");
  return indices;
}

function checkGlobals(payload, policy) {
  if (!payload) return;
  const state = { offset: 0 };
  const count = readU32(payload, state);
  if (count > policy.maxGlobals) fail(`module defines ${count} globals`);
  for (let index = 0; index < count; ++index) {
    const type = payload[state.offset++];
    const mutable = payload[state.offset++];
    if ((type !== 0x7f && type !== 0x7e) || mutable > 1) fail("invalid numeric global type");
    const opcode = payload[state.offset++];
    if (opcode === 0x41) skipLeb(payload, state, 5);
    else if (opcode === 0x42) skipLeb(payload, state, 10);
    else fail("global initializer must be an integer constant");
    if (payload[state.offset++] !== 0x0b) fail("malformed global initializer");
  }
  if (state.offset !== payload.length) fail("trailing bytes in global section");
}

function checkInstructions(bytes, state, end) {
  // Control-nesting depth is the one bound with no firmware counterpart: it
  // keeps this parser's own bookkeeping bounded on hostile input, and sits far
  // above anything a real effect reaches.
  let controlDepth = 1; // Function body is the outer control frame.
  while (state.offset < end) {
    const opcode = bytes[state.offset++];
    if (opcode === 0x02 || opcode === 0x03 || opcode === 0x04) {
      if (++controlDepth > 16) fail("control nesting exceeds the gate limit");
      const blockType = bytes[state.offset];
      if (blockType === 0x40 || blockType === 0x7f || blockType === 0x7e) state.offset++;
      else skipLeb(bytes, state, 5);
    } else if (opcode === 0x0c || opcode === 0x0d || opcode === 0x10 ||
               (opcode >= 0x20 && opcode <= 0x24)) {
      readU32(bytes, state);
    } else if (opcode === 0x0e) {
      const count = readU32(bytes, state);
      for (let index = 0; index <= count; ++index) readU32(bytes, state);
    } else if (opcode === 0x11 || opcode === 0x25 || opcode === 0x26) {
      fail("indirect calls and table instructions are not allowed");
    } else if (opcode === 0x1c) {
      const count = readU32(bytes, state);
      for (let index = 0; index < count; ++index) {
        const type = bytes[state.offset++];
        if (type !== 0x7f && type !== 0x7e) fail("typed select may use only i32 or i64");
      }
    } else if (opcode >= 0x28 && opcode <= 0x40) {
      fail("memory instructions are not allowed");
    } else if (opcode === 0x41) {
      skipLeb(bytes, state, 5);
    } else if (opcode === 0x42) {
      skipLeb(bytes, state, 10);
    } else if (opcode === 0x43 || opcode === 0x44 ||
               (opcode >= 0x5b && opcode <= 0x66) ||
               (opcode >= 0x8b && opcode <= 0xa6) ||
               (opcode >= 0xa8 && opcode <= 0xab) ||
               (opcode >= 0xae && opcode <= 0xbf) || opcode === 0xfc || opcode === 0xfd) {
      fail(`floating-point, saturating, bulk-memory, and SIMD opcode 0x${opcode.toString(16)} is not allowed`);
    } else if (opcode === 0xd0 || opcode === 0xd1 || opcode === 0xd2) {
      fail("reference instructions are not allowed");
    } else if (opcode === 0x0b) {
      if (--controlDepth < 0) fail("control stack underflow");
    } else if ([0x00, 0x01, 0x05, 0x0f, 0x1a, 0x1b].includes(opcode) ||
               (opcode >= 0x45 && opcode <= 0x5a) ||
               (opcode >= 0x67 && opcode <= 0x8a) ||
               (opcode >= 0xa7 && opcode <= 0xad) ||
               (opcode >= 0xc0 && opcode <= 0xc4)) {
      // Allowed integer/control instruction without an immediate.
    } else {
      fail(`unsupported opcode 0x${opcode.toString(16)}`);
    }
  }
  if (state.offset !== end) fail("instruction immediate extends beyond function body");
  if (controlDepth !== 0) fail("function body has unbalanced control frames");
}

function checkLocals(payload, definedCount, policy) {
  if (!payload) fail("missing code section");
  const state = { offset: 0 };
  const count = readU32(payload, state);
  if (count !== definedCount) fail("function and code section counts differ");
  for (let i = 0; i < count; ++i) {
    const bodySize = readU32(payload, state);
    if (bodySize > payload.length - state.offset) fail("function body extends beyond code section");
    const end = state.offset + bodySize;
    const groupCount = readU32(payload, state);
    let locals = 0;
    for (let group = 0; group < groupCount; ++group) {
      locals += readU32(payload, state);
      const type = payload[state.offset++];
      if (type !== 0x7f && type !== 0x7e) fail("locals may use only i32 and i64");
    }
    if (locals > policy.maxLocalsPerFunction) fail(`function ${i} defines ${locals} locals`);
    checkInstructions(payload, state, end);
  }
  if (state.offset !== payload.length) fail("trailing bytes in code section");
}

function checkExports(payload, types, functionTypes) {
  if (!payload) fail("missing export section");
  const state = { offset: 0 };
  const count = readU32(payload, state);
  if (count !== 2) fail(`expected two exports, found ${count}`);
  const wanted = [["rgbx_init", [], []], ["rgbx_tick", [0x7f], []]];
  for (const [name, params, results] of wanted) {
    if (readName(payload, state) !== name || payload[state.offset++] !== 0) {
      fail(`missing function export ${name}`);
    }
    const functionIndex = readU32(payload, state);
    if (!sameType(types[functionTypes[functionIndex]], params, results)) {
      fail(`wrong signature for ${name}`);
    }
  }
  if (state.offset !== payload.length) fail("trailing bytes in export section");
}

// Per-tick host-call budgets, taken from the pinned profile. The firmware
// traps a guest that exceeds any of them, so a module that would only stay
// inside them by luck is rejected here rather than shipped.
function executeOracle(bytes, importNames, policy) {
  const params = [50, 0x00ff40ff, 0, 0];
  let phase = "init";
  let nextPixel = 0;
  let paramCalls = 0;
  let inputCalls = 0;
  let spanCalls = 0;
  let goodMomentCalls = 0;
  let diagnostics = 0;
  const requireTick = (name) => {
    if (phase !== "tick") fail(`${name} is not allowed during rgbx_init`);
  };
  // Index bound per rgbx_v2_input_kind selector, in selector order: audio
  // bands, audio display buckets, beat mask, pressed buttons, accelerometer
  // axes, gyroscope axes, string length, string byte sum.
  const inputLimits = [
    policy.audioBandCount,
    policy.audioDisplayBucketCount,
    1,
    1,
    policy.imuAxisCount,
    policy.imuAxisCount,
    policy.maxStringParams,
    policy.maxStringParams,
  ];
  const validateInput = (kind, index) => {
    if ((kind >>> 0) >= inputLimits.length || (index >>> 0) >= inputLimits[kind >>> 0]) {
      fail(`invalid input selector ${kind}/${index}`);
    }
  };
  const beginSpan = (name, first, sampleCount) => {
    requireTick(name);
    if (++spanCalls > policy.spanCallsPerTick) fail("guest emitted more spans than one frame");
    if ((first >>> 0) !== nextPixel || sampleCount !== policy.pixelsPerSpan) {
      fail("invalid span sequence");
    }
    nextPixel += policy.pixelsPerSpan;
  };
  const host = {
    param_u32: (id) => {
      requireTick("param_u32");
      if ((id >>> 0) >= policy.maxParams || ++paramCalls > policy.maxParamCallsPerTick) {
        fail("invalid or excessive parameter read");
      }
      return params[id >>> 0] ?? 0;
    },
    input_u32: (kind, index) => {
      requireTick("input_u32");
      if (++inputCalls > policy.maxInputCallsPerTick) fail("excessive input read");
      validateInput(kind, index);
      return 0;
    },
    set_good_moment: (value) => {
      requireTick("set_good_moment");
      if ((value >>> 0) > 1 || ++goodMomentCalls > 1) fail("invalid good-moment call");
    },
    debug_u32: () => {
      requireTick("debug_u32");
      if (++diagnostics > policy.maxDiagnosticsPerTick) fail("too many diagnostic calls");
    },
    set_span8: (first, ...colors) => beginSpan("set_span8", first, colors.length),
    set_luma_span8: (first, foreground, background, ...lumas) => {
      if (lumas.some((value) => (value >>> 0) > 255)) fail("luma sample exceeds one byte");
      void foreground;
      void background;
      beginSpan("set_luma_span8", first, lumas.length);
    },
  };
  const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes), { rgbx_v2: host });
  instance.exports.rgbx_init();
  phase = "tick";

  // Probe several time and parameter combinations, not one fixed tick. A guest
  // that paints a complete frame only when dt equals a magic value, or only for
  // one parameter set, would slip past a single probe; each combination here
  // must independently paint every pixel exactly once.
  const paramSets = [
    [50, 0x00ff40ff, 0, 0],
    [0, 0x00000000, 1, 0x00ffffff],
    [0xffffffff, 0x00123456, 0, 1],
    [7, 0x00abcdef, 1, 0x00808080],
  ];
  const deltas = [0, 1, 17, 1000, 0xffffffff];
  for (let probe = 0; probe < deltas.length; ++probe) {
    for (let slot = 0; slot < params.length; ++slot) {
      params[slot] = paramSets[probe % paramSets.length][slot];
    }
    nextPixel = 0;
    paramCalls = 0;
    inputCalls = 0;
    spanCalls = 0;
    goodMomentCalls = 0;
    diagnostics = 0;
    instance.exports.rgbx_tick(deltas[probe]);
    if (nextPixel !== policy.pixelCount) {
      fail(`guest emitted ${nextPixel} pixels instead of ${policy.pixelCount} at dt=${deltas[probe]}`);
    }
    if (importNames.has("set_good_moment") && goodMomentCalls !== 1) {
      fail(`guest imported set_good_moment but did not call it exactly once at dt=${deltas[probe]}`);
    }
  }
}

// Structured clone cannot carry the derived Sets, and the worker must not
// re-read the manifest from disk: it receives exactly the profile the main
// thread already validated.
function serializablePolicy(policy) {
  const { allowedSections, requiredSections, ...rest } = policy;
  void allowedSections;
  void requiredSections;
  return rest;
}

function runOracleWithDeadline(bytes, importNames, policy) {
  return new Promise((resolvePromise, rejectPromise) => {
    let settled = false;
    const worker = new Worker(new URL(import.meta.url), {
      workerData: { bytes, importNames: [...importNames], policy: serializablePolicy(policy) },
      resourceLimits: ORACLE_RESOURCE_LIMITS,
      // The gate gives the guest no filesystem or network reach, but an empty
      // environment keeps a future host callback from becoming one.
      env: {},
    });
    const settle = (error) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      void worker.terminate();
      if (error) rejectPromise(error);
      else resolvePromise();
    };
    const timer = setTimeout(() => {
      settle(new Error(`RGBX v2 guest oracle exceeded ${ORACLE_DEADLINE_MS} ms`));
    }, ORACLE_DEADLINE_MS);
    worker.once("message", (message) => {
      settle(message.ok ? undefined : new Error(message.error));
    });
    worker.once("error", (error) => settle(error));
    // A worker that dies without reporting, whether out of memory against
    // the resource limits above or killed, must fail the gate, never pass it.
    worker.once("exit", (code) => {
      settle(new Error(`RGBX v2 guest oracle worker exited with code ${code}`));
    });
  });
}

async function main() {
  const [moduleArg] = process.argv.slice(2);
  if (!moduleArg) {
    console.error("usage: check-rgbx-v2.mjs <module.wasm>");
    process.exit(2);
  }
  const bytes = readFileSync(resolve(moduleArg));
  const policy = modulePolicy(loadSdkManifest());
  if (bytes.length < 8 || bytes.length > policy.moduleMaxBytes) {
    fail(`module size ${bytes.length} is outside 8..${policy.moduleMaxBytes}`);
  }
  const parsed = sections(bytes, policy);
  const types = parseTypes(parsed.get(1), policy);
  const imports = parseImports(parsed.get(2), types, policy);
  const definedTypes = parseFunctionTypes(parsed.get(3));
  if (imports.length + definedTypes.length > policy.maxFunctions) {
    fail(`module contains more than ${policy.maxFunctions} functions`);
  }
  checkGlobals(parsed.get(6), policy);
  checkLocals(parsed.get(10), definedTypes.length, policy);
  checkExports(parsed.get(7), types,
               [...imports.map((entry) => entry.typeIndex), ...definedTypes]);
  await runOracleWithDeadline(bytes, new Set(imports.map((entry) => entry.name)), policy);
  console.log(`RGBX v2 gate passed: ${bytes.length} bytes, ${imports.length} imports`);
}

if (isMainThread) {
  await main();
} else {
  try {
    executeOracle(Buffer.from(workerData.bytes), new Set(workerData.importNames),
                  workerData.policy);
    parentPort.postMessage({ ok: true });
  } catch (error) {
    parentPort.postMessage({ ok: false, error: error.message });
  }
}
