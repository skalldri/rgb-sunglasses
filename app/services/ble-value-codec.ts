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
