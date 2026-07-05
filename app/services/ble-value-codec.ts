import { METADATA_BLOB_VERSION } from '@/constants/bluetooth';

export type RgbColor = {
  r: number;
  g: number;
  b: number;
};

export function encodeBooleanToBase64(value: boolean): string {
  return btoa(String.fromCharCode(value ? 1 : 0));
}

export function decodeBooleanFromBase64(encodedValue?: string | null): boolean {
  if (!encodedValue) return false;
  const decoded = atob(encodedValue);
  return decoded.charCodeAt(0) !== 0;
}

export function encodeUtf8ToBase64(value: string): string {
  return btoa(value);
}

export function decodeUtf8FromBase64(encodedValue: string): string {
  return atob(encodedValue);
}

export function sanitizeNumericInput(value: string): string {
  return value.replace(/[^0-9]/g, '');
}

export function sanitizeFloatInput(value: string): string {
  // Strip everything but digits/'.'/'-', then collapse to at most one leading '-' and
  // one '.', mirroring how a native numeric-with-decimals keyboard would behave.
  const stripped = value.replace(/[^0-9.-]/g, '');
  const negative = stripped.startsWith('-');
  const digitsAndDot = stripped.replace(/-/g, '');
  const firstDotIndex = digitsAndDot.indexOf('.');
  const normalized =
    firstDotIndex === -1
      ? digitsAndDot
      : digitsAndDot.slice(0, firstDotIndex + 1) +
        digitsAndDot.slice(firstDotIndex + 1).replace(/\./g, '');
  return (negative ? '-' : '') + normalized;
}

export function encodeUint32ToBase64(value: number): string {
  const normalized = value >>> 0;
  const byte0 = normalized & 0xff;
  const byte1 = (normalized >> 8) & 0xff;
  const byte2 = (normalized >> 16) & 0xff;
  const byte3 = (normalized >> 24) & 0xff;
  return btoa(String.fromCharCode(byte0, byte1, byte2, byte3));
}

export function decodeUint32FromBase64(encodedValue: string): number {
  const decoded = atob(encodedValue);
  if (decoded.length < 4) {
    throw new Error(`Invalid uint32 payload length: ${decoded.length}`);
  }
  return (
    (decoded.charCodeAt(0) & 0xff) |
    ((decoded.charCodeAt(1) & 0xff) << 8) |
    ((decoded.charCodeAt(2) & 0xff) << 16) |
    ((decoded.charCodeAt(3) & 0xff) << 24)
  ) >>> 0;
}

export function decodeSint32FromBase64(encodedValue: string): number {
  const decoded = atob(encodedValue);
  if (decoded.length < 4) {
    throw new Error(`Invalid sint32 payload length: ${decoded.length}`);
  }
  // Same little-endian assembly as decodeUint32FromBase64, but signed: | 0 (instead
  // of >>> 0) keeps the result in two's-complement int32 range.
  return (
    (decoded.charCodeAt(0) & 0xff) |
    ((decoded.charCodeAt(1) & 0xff) << 8) |
    ((decoded.charCodeAt(2) & 0xff) << 16) |
    ((decoded.charCodeAt(3) & 0xff) << 24)
  ) | 0;
}

export function decodeUint8FromBase64(encodedValue: string): number {
  const decoded = atob(encodedValue);
  if (decoded.length < 1) {
    throw new Error(`Invalid uint8 payload length: ${decoded.length}`);
  }
  return decoded.charCodeAt(0) & 0xff;
}

export function encodeFloat32ToBase64(value: number): string {
  const buffer = new ArrayBuffer(4);
  new DataView(buffer).setFloat32(0, value, true /* littleEndian */);
  const bytes = new Uint8Array(buffer);
  return btoa(String.fromCharCode(bytes[0], bytes[1], bytes[2], bytes[3]));
}

export function decodeFloat32FromBase64(encodedValue: string): number {
  const decoded = atob(encodedValue);
  if (decoded.length < 4) {
    throw new Error(`Invalid float32 payload length: ${decoded.length}`);
  }
  const buffer = new ArrayBuffer(4);
  const bytes = new Uint8Array(buffer);
  for (let i = 0; i < 4; i++) {
    bytes[i] = decoded.charCodeAt(i) & 0xff;
  }
  return new DataView(buffer).getFloat32(0, true /* littleEndian */);
}

// float32 only carries ~7 significant decimal digits; stringifying a raw decoded value
// surfaces binary-rounding noise (e.g. 0.004999999888241291 instead of 0.005). Round-tripping
// through toPrecision strips that noise for display without losing real precision.
export function formatFloat32(value: number): string {
  return String(parseFloat(value.toPrecision(7)));
}

export function encodeColorToBase64(color: RgbColor): string {
  return btoa(String.fromCharCode(color.b & 0xff, color.g & 0xff, color.r & 0xff, 0));
}

export function decodeColorFromBase64(encodedValue?: string | null): RgbColor {
  if (!encodedValue) {
    return { r: 0, g: 0, b: 0 };
  }

  const decoded = atob(encodedValue);
  if (decoded.length < 3) {
    throw new Error(`Invalid color payload length: ${decoded.length}`);
  }

  return {
    b: decoded.charCodeAt(0) & 0xff,
    g: decoded.charCodeAt(1) & 0xff,
    r: decoded.charCodeAt(2) & 0xff,
  };
}

export type MetadataBlobEntry = {
  cpfFormat: number;
  name: string;
};

// Decodes the bulk per-service metadata characteristic's value (issue #41 follow-up - see
// UUID_METADATA_CHARACTERISTIC in constants/bluetooth.ts and MetadataBlobBuilder in
// fw/src/bluetooth/bt_service_cpp.h for the firmware side that produces this).
//
// Wire format: [version: 1 byte][entry_count: 1 byte], then entry_count repetitions of
// [cpf_format: 1 byte][name_len: 1 byte][name_bytes: name_len bytes, no NUL].
//
// Returns null on any version mismatch or malformed/truncated data, which the caller must
// treat as "fall back to the per-descriptor read path" - never zip a null result positionally.
// This deliberately does NOT validate entry_count against the service's actual characteristic
// count (the caller does that, since only it knows how many characteristics exist) - this
// function only validates that the bytes it was given are internally well-formed.
export function parseMetadataBlob(encodedValue?: string | null): MetadataBlobEntry[] | null {
  if (!encodedValue) return null;

  const decoded = atob(encodedValue);
  if (decoded.length < 2) return null;

  const version = decoded.charCodeAt(0);
  if (version !== METADATA_BLOB_VERSION) return null;

  const entryCount = decoded.charCodeAt(1);
  const entries: MetadataBlobEntry[] = [];
  let pos = 2;
  for (let i = 0; i < entryCount; i++) {
    if (pos + 2 > decoded.length) return null;
    const cpfFormat = decoded.charCodeAt(pos++);
    const nameLen = decoded.charCodeAt(pos++);
    if (pos + nameLen > decoded.length) return null;
    const name = decoded.slice(pos, pos + nameLen);
    pos += nameLen;
    entries.push({ cpfFormat, name });
  }

  return entries;
}
