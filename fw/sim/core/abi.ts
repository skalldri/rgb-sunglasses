/**
 * rgbx ABI constants and memory accessors for the WASM simulator.
 *
 * Byte offsets mirror fw/include/rgbx/rgbx_api.h compiled for wasm32
 * (ILP32, natural alignment — identical to the device's ARM EABI layout).
 * fw/sim/shim/abi_offsets.c proves the C side of these numbers at compile
 * time; the abi unit test proves this file agrees with a real module.
 */

export const RGBX_ABI_VERSION = 1;
export const RGBX_MAX_PARAMS = 16;
export const RGBX_MAX_STRING_PARAMS = 4;
export const RGBX_PARAM_STRING_MAX = 32;
export const RGBX_AUDIO_NUM_BANDS = 4;
export const RGBX_AUDIO_NUM_DISPLAY_BUCKETS = 20;

/** Number of buttons the host reports (kNumButtons in extension_host.cpp). */
export const NUM_BUTTONS = 5;
export const BUTTON_NAMES = ["Up", "Left", "Right", "Down", "Wake"] as const;

/** enum rgbx_param_type */
export enum RgbxParamType {
  Uint32 = 0,
  Color = 1,
  Bool = 2,
  String = 3,
  /** IEEE-754 float32 riding in the shared u32 params[] slot as its raw bit
   * pattern — never store an integer-truncated value for these (see
   * SimHost.setParamF32). */
  Float = 4,
}

/** struct rgbx_inputs field offsets (wasm32 == ARM EABI, ILP32). */
export const INPUTS = {
  dtMs: 0,
  params: 4,
  paramStrings: 68,
  accel: 196,
  gyro: 208,
  audioBandEnergy: 220,
  audioBeat: 236,
  audioDisplayBucket: 240,
  buttonsPressed: 320,
  size: 324,
} as const;

/** struct rgbx_manifest field offsets. */
export const MANIFEST = {
  abiVersion: 0,
  name: 4,
  width: 8,
  height: 12,
  paramCount: 16,
  params: 20,
  size: 24,
} as const;

/** struct rgbx_param_desc field offsets. */
export const PARAM_DESC = {
  name: 0,
  type: 4,
  defaultValue: 8,
  size: 12,
} as const;

/** The full per-tick input snapshot, in host-friendly form. */
export interface TickInputs {
  dtMs: number;
  /** 16 scalar values, COLOR params already mode-resolved to 0x00RRGGBB. */
  params: Uint32Array;
  /** 4 string slots, each at most RGBX_PARAM_STRING_MAX-1 bytes + NUL. */
  paramStrings: Uint8Array; // 4 * 32 bytes, NUL-terminated per slot
  accel: Float32Array; // 3, m/s^2
  gyro: Float32Array; // 3, rad/s
  audioBandEnergy: Float32Array; // 4
  audioBeat: Uint8Array; // 4, 0 or 1
  audioDisplayBucket: Float32Array; // 20
  buttonsPressed: number; // bitmask, bits 0..4
}

export function makeZeroInputs(dtMs: number): TickInputs {
  return {
    dtMs,
    params: new Uint32Array(RGBX_MAX_PARAMS),
    paramStrings: new Uint8Array(RGBX_MAX_STRING_PARAMS * RGBX_PARAM_STRING_MAX),
    accel: new Float32Array(3),
    gyro: new Float32Array(3),
    audioBandEnergy: new Float32Array(RGBX_AUDIO_NUM_BANDS),
    audioBeat: new Uint8Array(RGBX_AUDIO_NUM_BANDS),
    audioDisplayBucket: new Float32Array(RGBX_AUDIO_NUM_DISPLAY_BUCKETS),
    buttonsPressed: 0,
  };
}

/**
 * Serializes TickInputs into the extension's `rgbx_inputs` block at
 * `base` in linear memory — the simulator's version of the input-snapshot
 * write in extension_host::tick().
 */
export function writeInputs(memory: ArrayBuffer, base: number, inputs: TickInputs): void {
  const view = new DataView(memory);
  const u8 = new Uint8Array(memory);
  view.setUint32(base + INPUTS.dtMs, inputs.dtMs, true);
  for (let i = 0; i < RGBX_MAX_PARAMS; i++) {
    view.setUint32(base + INPUTS.params + i * 4, inputs.params[i], true);
  }
  u8.set(inputs.paramStrings, base + INPUTS.paramStrings);
  for (let i = 0; i < 3; i++) {
    view.setFloat32(base + INPUTS.accel + i * 4, inputs.accel[i], true);
    view.setFloat32(base + INPUTS.gyro + i * 4, inputs.gyro[i], true);
  }
  for (let b = 0; b < RGBX_AUDIO_NUM_BANDS; b++) {
    view.setFloat32(base + INPUTS.audioBandEnergy + b * 4, inputs.audioBandEnergy[b], true);
    u8[base + INPUTS.audioBeat + b] = inputs.audioBeat[b];
  }
  for (let i = 0; i < RGBX_AUDIO_NUM_DISPLAY_BUCKETS; i++) {
    view.setFloat32(
      base + INPUTS.audioDisplayBucket + i * 4,
      inputs.audioDisplayBucket[i],
      true,
    );
  }
  view.setUint32(base + INPUTS.buttonsPressed, inputs.buttonsPressed, true);
}

/** Encodes a JS string into string slot `slot` of a paramStrings buffer,
 * truncating to RGBX_PARAM_STRING_MAX-1 UTF-8 bytes, always NUL-terminated
 * (same bound the device enforces on BLE writes). */
export function setStringSlot(paramStrings: Uint8Array, slot: number, value: string): void {
  const bytes = new TextEncoder().encode(value).slice(0, RGBX_PARAM_STRING_MAX - 1);
  const off = slot * RGBX_PARAM_STRING_MAX;
  paramStrings.fill(0, off, off + RGBX_PARAM_STRING_MAX);
  paramStrings.set(bytes, off);
}

export function getStringSlot(paramStrings: Uint8Array, slot: number): string {
  const off = slot * RGBX_PARAM_STRING_MAX;
  let end = off;
  while (end < off + RGBX_PARAM_STRING_MAX && paramStrings[end] !== 0) {
    end++;
  }
  return new TextDecoder().decode(paramStrings.subarray(off, end));
}
