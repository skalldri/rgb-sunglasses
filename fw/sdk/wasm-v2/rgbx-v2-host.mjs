// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Stuart Alldritt
//
// The RGBX v2 admission profile, the structural admission pass, and the guest
// host, in one module with no platform dependencies.
//
// Three consumers run this exact code, which is the point of it being one
// module rather than three ports:
//
//   * the SDK's post-link gate (check-rgbx-v2.mjs), which admits a module and
//     then runs it against these per-tick budgets before it may be packaged;
//   * the simulator's Node CLI;
//   * the simulator's browser UI.
//
// It therefore imports nothing from node:, touches no filesystem, and uses no
// Buffer-only API: a bundler can put it in a browser worker unchanged.
//
// Every structural limit arrives in the `policy` argument, expanded by
// modulePolicy() from a manifest whose numbers the SDK packager copied out of
// <rgbx/rgbx_v2.h>. Nothing here carries a second opinion about a limit; the
// two values below are properties of the ABI's shape rather than tunables,
// and each says why.

/**
 * Thrown for every admission or host-contract violation raised here.
 *
 * `stage` separates the two failures a caller has to tell apart: "admission"
 * is a module the profile refuses, "init" is an admitted module whose
 * rgbx_init trapped. The firmware distinguishes them the same way, one at
 * policy-check time and one at m3_CallV time.
 */
export class RgbxV2Error extends Error {
  constructor(message, stage = "admission") {
    super(message);
    this.stage = stage;
  }
}

function fail(message, stage) {
  throw new RgbxV2Error(message, stage);
}

// ---------------------------------------------------------------------------
// Admission profile
// ---------------------------------------------------------------------------

// field -> [minimum, maximum] sanity window. These are not a second policy:
// they only reject a manifest that is corrupt or from an incompatible SDK, so
// a damaged pin cannot silently widen what the tools accept.
const REQUIRED_POLICY_FIELDS = {
  width: [1, 4096],
  height: [1, 4096],
  pixelsPerSpan: [1, 64],
  maxParams: [1, 256],
  maxStringParams: [0, 256],
  stringParamSize: [1, 4096],
  audioBandCount: [1, 256],
  audioDisplayBucketCount: [1, 256],
  imuAxisCount: [1, 16],
  maxDiagnosticsPerTick: [0, 256],
  moduleMaxBytes: [8, 1 << 20],
  maxFunctions: [1, 1024],
  maxGlobals: [0, 1024],
  maxLocalsPerFunction: [0, 1024],
  minImports: [0, 64],
  maxImports: [0, 64],
  maxParamCallsPerTick: [0, 4096],
  maxInputCallsPerTick: [0, 4096],
  sectionAllowedMask: [1, 0xffffffff],
  sectionRequiredMask: [1, 0xffffffff],
};

function requireInteger(value, name, [minimum, maximum]) {
  if (!Number.isInteger(value) || value < minimum || value > maximum) {
    fail(`sdk-manifest ${name} must be an integer in ${minimum}..${maximum}`);
  }
  return value;
}

function maskToSet(mask, name) {
  const ids = new Set();
  for (let id = 0; id < 32; ++id) {
    if ((mask & (1 << id)) !== 0) ids.add(id);
  }
  if (ids.size === 0) fail(`sdk-manifest ${name} selects no section ids`);
  return ids;
}

/**
 * Validate and expand the RGBX v2 admission profile carried by a manifest.
 *
 * Returns the pinned fields plus the values every consumer derives the same
 * way, so no caller re-derives a frame geometry of its own.
 */
export function modulePolicy(manifest) {
  const pinned = manifest.rgbxV2ModulePolicy;
  if (pinned === null || typeof pinned !== "object" || Array.isArray(pinned)) {
    fail("sdk-manifest rgbxV2ModulePolicy is missing; this SDK predates the pinned v2 profile");
  }
  const policy = {};
  for (const [name, range] of Object.entries(REQUIRED_POLICY_FIELDS)) {
    policy[name] = requireInteger(pinned[name], `rgbxV2ModulePolicy.${name}`, range);
  }
  if (policy.minImports > policy.maxImports) {
    fail("sdk-manifest rgbxV2ModulePolicy.minImports exceeds maxImports");
  }
  if ((policy.sectionRequiredMask & ~policy.sectionAllowedMask) !== 0) {
    fail("sdk-manifest rgbxV2ModulePolicy requires a section id it does not admit");
  }

  const capabilityBits = pinned.capabilityBits;
  if (capabilityBits === null || typeof capabilityBits !== "object" ||
      Array.isArray(capabilityBits)) {
    fail("sdk-manifest rgbxV2ModulePolicy.capabilityBits must be an object");
  }
  for (const [name, bit] of Object.entries(capabilityBits)) {
    if (!/^[a-z][a-z0-9_]*$/.test(name) || !Number.isInteger(bit) || bit <= 0 ||
        (bit & (bit - 1)) !== 0) {
      fail(`sdk-manifest capability ${name} must map to a single power-of-two bit`);
    }
  }

  const pixelCount = policy.width * policy.height;
  if (pixelCount % policy.pixelsPerSpan !== 0) {
    fail("sdk-manifest frame geometry is not an exact multiple of the span width");
  }
  return {
    ...policy,
    capabilityBits,
    pixelCount,
    spanCallsPerTick: pixelCount / policy.pixelsPerSpan,
    allowedSections: maskToSet(policy.sectionAllowedMask, "sectionAllowedMask"),
    requiredSections: maskToSet(policy.sectionRequiredMask, "sectionRequiredMask"),
  };
}

/** Every capability bit the profile defines, as one mask. */
export function allCapabilities(policy) {
  let mask = 0;
  for (const bit of Object.values(policy.capabilityBits)) mask |= bit;
  return mask >>> 0;
}

// ---------------------------------------------------------------------------
// Structural admission
// ---------------------------------------------------------------------------

const utf8 = new TextDecoder("utf-8", { fatal: false });

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
  const value = utf8.decode(bytes.subarray(state.offset, state.offset + length));
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

const WASM_MAGIC = [0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00];

/**
 * Re-derive the firmware's admission decision from the module bytes.
 *
 * Throws RgbxV2Error naming the first failed check; on success returns the
 * import shape the host needs, which is the same thing the firmware records
 * while it walks the module before linking a single import.
 *
 * @param {Uint8Array} bytes Complete module.
 * @param {object} policy Expanded profile from modulePolicy().
 */
export function admitModule(bytes, policy) {
  if (bytes.length < WASM_MAGIC.length || bytes.length > policy.moduleMaxBytes) {
    fail(`module size ${bytes.length} is outside ${WASM_MAGIC.length}..${policy.moduleMaxBytes}`);
  }
  for (let i = 0; i < WASM_MAGIC.length; ++i) {
    if (bytes[i] !== WASM_MAGIC[i]) fail("input is not a WebAssembly v1 module");
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
  const importNames = new Set(imports.map((entry) => entry.name));
  return {
    importNames,
    importCount: imports.length,
    spanEncoding: importNames.has("set_span8") ? "rgb24" : "paletteLuma",
  };
}

// ---------------------------------------------------------------------------
// Guest host
// ---------------------------------------------------------------------------

/**
 * Blend one palette-luma sample, bit-for-bit as the firmware's blendLuma().
 *
 * Integer arithmetic with C truncation toward zero throughout: the rounding
 * is visible on the panel, so an innocent-looking Math.round here would put
 * the simulator a channel step away from the device on half the pixels.
 *
 * @param {number} foreground 0x00RRGGBB selected at luma 255.
 * @param {number} background 0x00RRGGBB selected at luma 0.
 * @param {number} luma Sample in the closed range 0..255.
 */
export function blendLuma(foreground, background, luma) {
  const fg = foreground & 0x00ffffff;
  const bg = background & 0x00ffffff;
  const amount = luma | 0;
  const channel = (shift) => {
    const from = (bg >>> shift) & 0xff;
    const to = (fg >>> shift) & 0xff;
    return (from + Math.trunc(((to - from) * amount) / 255)) & 0xff;
  };
  return ((channel(16) << 16) | (channel(8) << 8) | channel(0)) >>> 0;
}

/** Selector values of enum rgbx_v2_input_kind, in ABI order. */
export const INPUT_KIND = {
  audioBandQ16: 0,
  audioDisplayQ16: 1,
  audioBeatMask: 2,
  buttonsPressed: 3,
  accelMilli: 4,
  gyroMilli: 5,
  stringLength: 6,
  stringByteSum: 7,
};

// Which capability each sensor selector needs. The names are the ABI header's
// own RGBX_V2_CAPABILITY_* spellings, lowercased, which is exactly what the
// profile's capability table is keyed by. A selector missing from this table
// is ungated -- the string summaries, which read this extension's own
// parameters rather than a sensor.
const INPUT_CAPABILITY = {
  [INPUT_KIND.audioBandQ16]: "audio",
  [INPUT_KIND.audioDisplayQ16]: "audio",
  [INPUT_KIND.audioBeatMask]: "audio",
  [INPUT_KIND.buttonsPressed]: "buttons",
  [INPUT_KIND.accelMilli]: "imu",
  [INPUT_KIND.gyroMilli]: "imu",
};

/**
 * The permission bit a capability name maps to in this profile.
 *
 * A name the profile does not declare is refused by name rather than read as
 * undefined: `capabilities & undefined` is 0, which would silently deny every
 * guest instead of reporting that the host and the ABI header disagree about
 * what the capability is called.
 */
function capabilityBit(policy, name) {
  const bit = policy.capabilityBits[name];
  if (!Number.isInteger(bit) || bit <= 0) {
    fail(`the admission profile declares no "${name}" capability bit`);
  }
  return bit;
}

/** Zeroed tick inputs shaped for RgbxV2Guest.tick(). */
export function makeV2Inputs(policy) {
  return {
    params: new Uint32Array(policy.maxParams),
    // One NUL-terminated slot per string parameter, stringParamSize bytes each,
    // exactly like the device's fixed paramStrings storage.
    paramStrings: new Uint8Array(policy.maxStringParams * policy.stringParamSize),
    accelMilli: new Int32Array(policy.imuAxisCount),
    gyroMilli: new Int32Array(policy.imuAxisCount),
    audioBandQ16: new Uint32Array(policy.audioBandCount),
    audioDisplayQ16: new Uint32Array(policy.audioDisplayBucketCount),
    audioBeatMask: 0,
    buttonsPressed: 0,
  };
}

/**
 * One admitted, instantiated RGBX v2 guest and the host it runs against.
 *
 * The host reproduces the firmware's import surface exactly: the same call
 * quotas per tick, the same ordered span sequence, the same fail-closed
 * capability gate (an ungranted sensor read is refused and traps the tick, it
 * does not quietly return zero), and the same generation-atomic commit -- a
 * frame becomes visible only when the tick completed, painted every pixel
 * once, and published its good moment if it imported one.
 */
export class RgbxV2Guest {
  /**
   * @param {Uint8Array} bytes Module bytes, already read from a package.
   * @param {object} policy Expanded profile from modulePolicy().
   * @param {object} [options] `capabilities` is the granted permission mask.
   */
  constructor(bytes, policy, options = {}) {
    this.policy = policy;
    this.capabilities = (options.capabilities ?? 0) >>> 0;
    const admitted = admitModule(bytes, policy);
    this.importNames = admitted.importNames;
    this.spanEncoding = admitted.spanEncoding;

    this.pending = new Uint32Array(policy.pixelCount);
    this.committed = new Uint32Array(policy.pixelCount);
    this.diagnostics = [];
    this.goodMoment = 1;
    this.inputs = makeV2Inputs(policy);

    // Resolved once, so a profile whose capability names have moved is
    // reported here rather than turning into a silent denial at read time.
    this.inputCapability = new Map(
      Object.entries(INPUT_CAPABILITY).map(([kind, capability]) =>
        [Number(kind), capabilityBit(policy, capability)]),
    );

    this.phase = "init";
    this.rejection = null;
    this.resetTickState();

    const instance = new WebAssembly.Instance(
      new WebAssembly.Module(bytes),
      { rgbx_v2: this.buildImports() },
    );
    this.instance = instance;
    const call = this.guard(() => instance.exports.rgbx_init());
    if (call.rejection !== null) fail(call.rejection, "init");
    if (call.trap !== null) fail(`rgbx_init trapped: ${call.trap}`, "init");
    this.phase = "idle";
  }

  /** Per-tick counters the firmware clears before every rgbx_tick call. */
  resetTickState() {
    this.paramCalls = 0;
    this.inputCalls = 0;
    this.spanCalls = 0;
    this.goodMomentCalls = 0;
    this.diagnostics = [];
    this.pending.fill(0);
    this.goodMoment = this.importNames.has("set_good_moment") ? 0 : 1;
  }

  /** Refuse the call the way the firmware's host imports refuse it. */
  reject(reason) {
    if (this.rejection === null) this.rejection = reason;
    throw new RgbxV2Error(reason);
  }

  requireTick(name) {
    // The firmware stamps a zero request generation outside a tick, and every
    // import refuses on it -- so a guest cannot paint or read a sensor from
    // rgbx_init, where no input snapshot exists yet.
    if (this.phase !== "tick") this.reject(`${name} is not allowed during rgbx_init`);
  }

  /** Run one guest entry point, separating a host refusal from a plain trap. */
  guard(call) {
    this.rejection = null;
    try {
      call();
    } catch (error) {
      const trap = this.rejection === null ? String(error) : null;
      return { rejection: this.rejection, trap };
    }
    return { rejection: null, trap: null };
  }

  stringSlot(index) {
    const size = this.policy.stringParamSize;
    return this.inputs.paramStrings.subarray(index * size, (index + 1) * size);
  }

  readInput(kind, index) {
    const policy = this.policy;
    const inputs = this.inputs;
    // Capability before index, exactly as the firmware orders it: a guest
    // must not learn a bound for a sensor it was never granted.
    const required = this.inputCapability.get(kind);
    if (required !== undefined && (this.capabilities & required) === 0) {
      this.reject(`${INPUT_CAPABILITY[kind]} capability was not granted`);
    }
    switch (kind) {
      case INPUT_KIND.audioBandQ16:
        if (index >= policy.audioBandCount) this.reject(`audio band ${index} is out of range`);
        return inputs.audioBandQ16[index];
      case INPUT_KIND.audioDisplayQ16:
        if (index >= policy.audioDisplayBucketCount) {
          this.reject(`audio display bucket ${index} is out of range`);
        }
        return inputs.audioDisplayQ16[index];
      case INPUT_KIND.audioBeatMask:
        if (index !== 0) this.reject("the beat mask has one index");
        return inputs.audioBeatMask;
      case INPUT_KIND.buttonsPressed:
        if (index !== 0) this.reject("the pressed-button mask has one index");
        return inputs.buttonsPressed;
      case INPUT_KIND.accelMilli:
        if (index >= policy.imuAxisCount) this.reject(`accelerometer axis ${index} is out of range`);
        return inputs.accelMilli[index];
      case INPUT_KIND.gyroMilli:
        if (index >= policy.imuAxisCount) this.reject(`gyroscope axis ${index} is out of range`);
        return inputs.gyroMilli[index];
      case INPUT_KIND.stringLength:
      case INPUT_KIND.stringByteSum: {
        // String summaries are not capability-gated on the device: the values
        // come from this extension's own parameters, not from a sensor.
        if (index >= policy.maxStringParams) this.reject(`string slot ${index} is out of range`);
        const slot = this.stringSlot(index);
        let value = 0;
        for (let i = 0; i < slot.length && slot[i] !== 0; ++i) {
          value += kind === INPUT_KIND.stringLength ? 1 : slot[i];
        }
        return value;
      }
      default:
        return this.reject(`unknown input selector ${kind}`);
    }
  }

  beginSpan(name, firstPixel, sampleCount) {
    const policy = this.policy;
    this.requireTick(name);
    if (this.spanCalls >= policy.spanCallsPerTick) {
      this.reject("guest emitted more spans than one frame");
    }
    if (firstPixel !== this.spanCalls * policy.pixelsPerSpan ||
        sampleCount !== policy.pixelsPerSpan) {
      this.reject(`span at ${firstPixel} is out of the ordered frame sequence`);
    }
    ++this.spanCalls;
  }

  buildImports() {
    const policy = this.policy;
    return {
      param_u32: (id) => {
        this.requireTick("param_u32");
        const slot = id >>> 0;
        if (slot >= policy.maxParams) this.reject(`parameter ${slot} is out of range`);
        if (++this.paramCalls > policy.maxParamCallsPerTick) {
          this.reject("guest exceeded its per-tick parameter reads");
        }
        return this.inputs.params[slot];
      },
      input_u32: (kind, index) => {
        this.requireTick("input_u32");
        if (!this.importNames.has("input_u32")) this.reject("input_u32 was not imported");
        if (++this.inputCalls > policy.maxInputCallsPerTick) {
          this.reject("guest exceeded its per-tick input reads");
        }
        return this.readInput(kind >>> 0, index >>> 0) >>> 0;
      },
      set_good_moment: (value) => {
        this.requireTick("set_good_moment");
        if ((value >>> 0) > 1 || ++this.goodMomentCalls > 1) {
          this.reject("the good-moment signal takes one 0 or 1 per frame");
        }
        this.goodMoment = value >>> 0;
      },
      debug_u32: (tag, value) => {
        this.requireTick("debug_u32");
        if (this.diagnostics.length >= policy.maxDiagnosticsPerTick) {
          this.reject("guest exceeded its per-tick diagnostics");
        }
        this.diagnostics.push({ tag: tag >>> 0, value: value >>> 0 });
      },
      set_span8: (firstPixel, ...colors) => {
        this.beginSpan("set_span8", firstPixel >>> 0, colors.length);
        const first = firstPixel >>> 0;
        for (let i = 0; i < colors.length; ++i) {
          this.pending[first + i] = colors[i] & 0x00ffffff;
        }
      },
      set_luma_span8: (firstPixel, foreground, background, ...lumas) => {
        // Luma range is checked before the sequence, exactly as the firmware
        // folds it into the same refusal.
        if (lumas.some((value) => (value >>> 0) > 255)) {
          this.reject("luma sample exceeds one byte");
        }
        this.beginSpan("set_luma_span8", firstPixel >>> 0, lumas.length);
        const first = firstPixel >>> 0;
        for (let i = 0; i < lumas.length; ++i) {
          this.pending[first + i] = blendLuma(foreground, background, lumas[i] >>> 0);
        }
      },
    };
  }

  /**
   * Run one tick.
   *
   * Mutate `guest.inputs` first: it is the tick snapshot the imports read,
   * the direct analog of the firmware's shared mailbox. The returned frame is
   * the committed buffer, which only advances when the tick was accepted.
   *
   * @param {number} dtMs Elapsed milliseconds passed to rgbx_tick.
   * @returns {{ok: boolean, reason: string|null, kind: string,
   *            pixels: Uint32Array, goodMoment: boolean, diagnostics: Array}}
   */
  tick(dtMs) {
    this.resetTickState();
    this.phase = "tick";
    const call = this.guard(() => this.instance.exports.rgbx_tick(dtMs >>> 0));
    this.phase = "idle";

    let kind = "ok";
    let reason = null;
    if (call.rejection !== null) {
      kind = "rejected";
      reason = call.rejection;
    } else if (call.trap !== null) {
      kind = "trap";
      reason = call.trap;
    } else if (this.spanCalls !== this.policy.spanCallsPerTick) {
      kind = "incomplete_frame";
      reason = `guest emitted ${this.spanCalls * this.policy.pixelsPerSpan} pixels ` +
               `instead of ${this.policy.pixelCount}`;
    } else if (this.importNames.has("set_good_moment") && this.goodMomentCalls !== 1) {
      kind = "incomplete_frame";
      reason = "guest imported set_good_moment but did not call it exactly once";
    }

    if (kind === "ok") {
      // Generation-atomic commit: a partially painted frame never reaches a
      // consumer, so the panel holds the last complete frame instead of
      // tearing (fw/src/extensions/wasm_mvp_runtime.cpp).
      this.committed.set(this.pending);
    }
    return {
      ok: kind === "ok",
      kind,
      reason,
      pixels: this.committed,
      goodMoment: this.goodMoment !== 0,
      diagnostics: this.diagnostics,
    };
  }
}
