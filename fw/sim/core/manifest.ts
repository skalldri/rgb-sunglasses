/**
 * TypeScript port of the firmware's manifest validator
 * (fw/src/extensions/extension_manifest.{h,cpp}).
 *
 * Result names and check ORDER are kept in lockstep with the C++ so a
 * rejection here matches the device's log message for the same defect. One
 * documented divergence: the region predicate is "wholly inside linear
 * memory" — weaker than llext region membership (see PARITY.md).
 */

import { MANIFEST, PARAM_DESC, RGBX_ABI_VERSION, RGBX_MAX_PARAMS, RGBX_MAX_STRING_PARAMS, RGBX_PARAM_STRING_MAX, RgbxParamType } from "./abi";

/** Mirrors extension_manifest::Result (names must not drift). */
export enum ManifestResult {
  Ok = "Ok",
  BadManifestPointer = "BadManifestPointer",
  BadAbiVersion = "BadAbiVersion",
  BadDims = "BadDims",
  BadParamTable = "BadParamTable",
  BadName = "BadName",
  BadParamName = "BadParamName",
  BadParamType = "BadParamType",
  TooManyStringParams = "TooManyStringParams",
  BadStringDefault = "BadStringDefault",
}

/** Mirrors extension_manifest::result_str(). */
export function manifestResultStr(r: ManifestResult): string {
  switch (r) {
    case ManifestResult.Ok: return "ok";
    case ManifestResult.BadManifestPointer: return "manifest outside extension memory";
    case ManifestResult.BadAbiVersion: return "ABI version mismatch";
    case ManifestResult.BadDims: return "framebuffer dims don't match display";
    case ManifestResult.BadParamTable: return "bad param table";
    case ManifestResult.BadName: return "bad manifest name";
    case ManifestResult.BadParamName: return "bad param name";
    case ManifestResult.BadParamType: return "bad param type";
    case ManifestResult.TooManyStringParams: return "too many string params";
    case ManifestResult.BadStringDefault: return "bad string param default";
  }
}

/** kStringScanMax in extension_manifest.cpp. */
const STRING_SCAN_MAX = 256;
/** extension_host::kMaxNameLen == kAnimationNameMaxLen (animation_types.h). */
export const MAX_NAME_LEN = 24;
/** extension_host::kMaxParamNameLen (extension_limits.h). */
export const MAX_PARAM_NAME_LEN = 20;
/** extension_manifest::kNoStringSlot. */
export const NO_STRING_SLOT = 0xff;

export interface ParamInfo {
  name: string; // truncated to MAX_PARAM_NAME_LEN-1 bytes like the device
  type: RgbxParamType;
  defaultValue: number; // scalar types; BOOL clamped to 0/1
  stringSlot: number; // NO_STRING_SLOT for non-string params
}

/** Mirrors extension_manifest::Metadata — no pointers into extension
 * memory survive parsing. */
export interface ManifestMetadata {
  displayName: string;
  width: number;
  height: number;
  paramCount: number;
  stringParamCount: number;
  params: ParamInfo[];
  /** Defaults per string slot, already validated < RGBX_PARAM_STRING_MAX. */
  stringDefaults: string[];
}

export interface ValidateEnv {
  expectedWidth: number;
  expectedHeight: number;
}

export type ValidateOutcome =
  | { result: ManifestResult.Ok; metadata: ManifestMetadata }
  | { result: Exclude<ManifestResult, ManifestResult.Ok>; metadata?: undefined };

/** Copies a NUL-terminated string out of untrusted extension memory:
 * truncates to cap-1 bytes, but a NUL must exist within STRING_SCAN_MAX
 * in-bounds bytes (copy_untrusted_string). Returns null on rejection. */
function copyUntrustedString(mem: Uint8Array, ptr: number, cap: number): string | null {
  for (let i = 0; i < STRING_SCAN_MAX; i++) {
    if (ptr + i >= mem.length) {
      return null; // ran off the region
    }
    if (mem[ptr + i] === 0) {
      const n = Math.min(i, cap - 1);
      return new TextDecoder().decode(mem.subarray(ptr, ptr + n));
    }
  }
  return null; // no NUL within the scan bound
}

function inRegion(mem: Uint8Array, ptr: number, len: number): boolean {
  return ptr >= 0 && len >= 0 && ptr + len <= mem.length;
}

/**
 * Port of extension_manifest::validate(). `manifestAddr` is the value of
 * the module's exported `rgbx_manifest` global.
 */
export function validateManifest(
  memory: ArrayBuffer,
  manifestAddr: number,
  env: ValidateEnv,
): ValidateOutcome {
  const mem = new Uint8Array(memory);
  const view = new DataView(memory);

  /* Nothing may be read from the manifest before this check. */
  if (manifestAddr === 0 || !inRegion(mem, manifestAddr, MANIFEST.size)) {
    return { result: ManifestResult.BadManifestPointer };
  }

  const abiVersion = view.getUint32(manifestAddr + MANIFEST.abiVersion, true);
  if (abiVersion !== RGBX_ABI_VERSION) {
    return { result: ManifestResult.BadAbiVersion };
  }

  const width = view.getUint32(manifestAddr + MANIFEST.width, true);
  const height = view.getUint32(manifestAddr + MANIFEST.height, true);
  if (width !== env.expectedWidth || height !== env.expectedHeight) {
    return { result: ManifestResult.BadDims };
  }

  const paramCount = view.getUint32(manifestAddr + MANIFEST.paramCount, true);
  const paramsPtr = view.getUint32(manifestAddr + MANIFEST.params, true);
  if (paramCount > RGBX_MAX_PARAMS) {
    return { result: ManifestResult.BadParamTable };
  }
  if ((paramCount === 0) !== (paramsPtr === 0)) {
    return { result: ManifestResult.BadParamTable };
  }
  if (paramCount > 0 && !inRegion(mem, paramsPtr, paramCount * PARAM_DESC.size)) {
    return { result: ManifestResult.BadParamTable };
  }

  const namePtr = view.getUint32(manifestAddr + MANIFEST.name, true);
  let displayName: string;
  if (namePtr === 0) {
    displayName = "unnamed";
  } else {
    const copied = copyUntrustedString(mem, namePtr, MAX_NAME_LEN);
    if (copied === null) {
      return { result: ManifestResult.BadName };
    }
    displayName = copied;
  }

  const params: ParamInfo[] = [];
  const stringDefaults: string[] = [];

  for (let p = 0; p < paramCount; p++) {
    const descAddr = paramsPtr + p * PARAM_DESC.size;
    const descNamePtr = view.getUint32(descAddr + PARAM_DESC.name, true);
    const type = view.getUint32(descAddr + PARAM_DESC.type, true);
    const defaultRaw = view.getUint32(descAddr + PARAM_DESC.defaultValue, true);

    let name: string;
    if (descNamePtr === 0) {
      name = "param";
    } else {
      const copied = copyUntrustedString(mem, descNamePtr, MAX_PARAM_NAME_LEN);
      if (copied === null) {
        return { result: ManifestResult.BadParamName };
      }
      name = copied;
    }

    const info: ParamInfo = { name, type, defaultValue: 0, stringSlot: NO_STRING_SLOT };

    switch (type) {
      case RgbxParamType.Uint32:
      case RgbxParamType.Color:
        info.defaultValue = defaultRaw;
        break;
      case RgbxParamType.Bool:
        info.defaultValue = defaultRaw !== 0 ? 1 : 0;
        break;
      case RgbxParamType.String: {
        if (stringDefaults.length >= RGBX_MAX_STRING_PARAMS) {
          return { result: ManifestResult.TooManyStringParams };
        }
        let def = "";
        if (defaultRaw !== 0) {
          /* Defaults must round-trip through BLE unmodified, so overlong
           * ones are rejected rather than truncated. */
          const copied = copyUntrustedString(mem, defaultRaw, STRING_SCAN_MAX);
          if (copied === null || byteLength(copied) >= RGBX_PARAM_STRING_MAX) {
            return { result: ManifestResult.BadStringDefault };
          }
          def = copied;
        }
        info.stringSlot = stringDefaults.length;
        stringDefaults.push(def);
        break;
      }
      default:
        return { result: ManifestResult.BadParamType };
    }
    params.push(info);
  }

  return {
    result: ManifestResult.Ok,
    metadata: {
      displayName,
      width,
      height,
      paramCount,
      stringParamCount: stringDefaults.length,
      params,
      stringDefaults,
    },
  };
}

function byteLength(s: string): number {
  return new TextEncoder().encode(s).length;
}
