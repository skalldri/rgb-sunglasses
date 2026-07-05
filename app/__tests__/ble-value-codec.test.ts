import { METADATA_BLOB_VERSION } from '@/constants/bluetooth';
import {
  decodeBooleanFromBase64,
  decodeColorFromBase64,
  decodeFloat32FromBase64,
  decodeSint32FromBase64,
  decodeUint32FromBase64,
  decodeUint8FromBase64,
  decodeUtf8FromBase64,
  encodeBooleanToBase64,
  encodeColorToBase64,
  encodeFloat32ToBase64,
  encodeUint32ToBase64,
  encodeUtf8ToBase64,
  MetadataBlobEntry,
  parseMetadataBlob,
  sanitizeFloatInput,
  sanitizeNumericInput,
} from '@/services/ble-value-codec';

// Mirrors the wire format documented on parseMetadataBlob(): [version][entry_count], then
// entry_count repetitions of [cpf_format][name_len][name_bytes]. Lets tests build valid blobs
// without duplicating byte-packing logic, and `overrides` lets tests deliberately corrupt a
// well-formed blob (e.g. truncate it) to exercise the malformed-input paths.
function buildMetadataBlobBase64(
  entries: MetadataBlobEntry[],
  overrides: { version?: number; entryCount?: number; truncateToLength?: number } = {}
): string {
  const bytes: number[] = [
    overrides.version ?? METADATA_BLOB_VERSION,
    overrides.entryCount ?? entries.length,
  ];
  for (const entry of entries) {
    bytes.push(entry.cpfFormat, entry.name.length, ...Array.from(entry.name, c => c.charCodeAt(0)));
  }
  const truncated = overrides.truncateToLength !== undefined ? bytes.slice(0, overrides.truncateToLength) : bytes;
  return btoa(String.fromCharCode(...truncated));
}

describe('ble-value-codec', () => {
  it('encodes and decodes boolean values', () => {
    const trueEncoded = encodeBooleanToBase64(true);
    const falseEncoded = encodeBooleanToBase64(false);

    expect(decodeBooleanFromBase64(trueEncoded)).toBe(true);
    expect(decodeBooleanFromBase64(falseEncoded)).toBe(false);
    expect(decodeBooleanFromBase64(null)).toBe(false);
    expect(decodeBooleanFromBase64(undefined)).toBe(false);
  });

  it('encodes and decodes utf8 values', () => {
    const encoded = encodeUtf8ToBase64('hello world');
    expect(decodeUtf8FromBase64(encoded)).toBe('hello world');
  });

  it('sanitizes numeric input', () => {
    expect(sanitizeNumericInput('12ab3')).toBe('123');
    expect(sanitizeNumericInput('')).toBe('');
    expect(sanitizeNumericInput('0x123')).toBe('0123');
  });

  it('encodes and decodes uint32 values', () => {
    const encoded = encodeUint32ToBase64(1234567890);
    expect(decodeUint32FromBase64(encoded)).toBe(1234567890);

    const maxEncoded = encodeUint32ToBase64(0xffffffff);
    expect(decodeUint32FromBase64(maxEncoded)).toBe(0xffffffff);
  });

  it('throws for invalid uint32 payloads', () => {
    expect(() => decodeUint32FromBase64(btoa('abc'))).toThrow('Invalid uint32 payload length');
  });

  it('sanitizes float input', () => {
    expect(sanitizeFloatInput('12.3ab4')).toBe('12.34');
    expect(sanitizeFloatInput('-0.5')).toBe('-0.5');
    expect(sanitizeFloatInput('1.2.3')).toBe('1.23');
    expect(sanitizeFloatInput('--1.5')).toBe('-1.5');
    expect(sanitizeFloatInput('')).toBe('');
  });

  it('encodes and decodes float32 values', () => {
    const encoded = encodeFloat32ToBase64(3.5);
    expect(decodeFloat32FromBase64(encoded)).toBeCloseTo(3.5, 5);

    const smallEncoded = encodeFloat32ToBase64(0.005);
    expect(decodeFloat32FromBase64(smallEncoded)).toBeCloseTo(0.005, 5);

    const negativeEncoded = encodeFloat32ToBase64(-20.0);
    expect(decodeFloat32FromBase64(negativeEncoded)).toBeCloseTo(-20.0, 5);

    const zeroEncoded = encodeFloat32ToBase64(0);
    expect(decodeFloat32FromBase64(zeroEncoded)).toBe(0);
  });

  it('throws for invalid float32 payloads', () => {
    expect(() => decodeFloat32FromBase64(btoa('abc'))).toThrow('Invalid float32 payload length');
  });

  it('encodes and decodes color values', () => {
    const encoded = encodeColorToBase64({ r: 10, g: 20, b: 30 });
    expect(decodeColorFromBase64(encoded)).toEqual({ r: 10, g: 20, b: 30 });
    expect(decodeColorFromBase64(undefined)).toEqual({ r: 0, g: 0, b: 0 });
    expect(decodeColorFromBase64(null)).toEqual({ r: 0, g: 0, b: 0 });
  });

  it('throws for invalid color payloads', () => {
    expect(() => decodeColorFromBase64(btoa('ab'))).toThrow('Invalid color payload length');
  });

  it('decodes signed 32-bit values, little-endian', () => {
    // encodeUint32ToBase64(v >>> 0) produces the two's-complement wire bytes for
    // negative values, matching what the firmware's int32 characteristics send.
    expect(decodeSint32FromBase64(encodeUint32ToBase64(7910))).toBe(7910);
    expect(decodeSint32FromBase64(encodeUint32ToBase64(0))).toBe(0);
    expect(decodeSint32FromBase64(encodeUint32ToBase64(-350 >>> 0))).toBe(-350);
    expect(decodeSint32FromBase64(encodeUint32ToBase64(-1 >>> 0))).toBe(-1);
    expect(decodeSint32FromBase64(encodeUint32ToBase64(2147483647))).toBe(2147483647);
    expect(decodeSint32FromBase64(encodeUint32ToBase64(-2147483648 >>> 0))).toBe(-2147483648);
  });

  it('throws for short sint32 payloads', () => {
    expect(() => decodeSint32FromBase64(btoa('ab'))).toThrow('Invalid sint32 payload length');
  });

  it('decodes uint8 values', () => {
    expect(decodeUint8FromBase64(btoa(String.fromCharCode(0)))).toBe(0);
    expect(decodeUint8FromBase64(btoa(String.fromCharCode(3)))).toBe(3);
    expect(decodeUint8FromBase64(btoa(String.fromCharCode(255)))).toBe(255);
  });

  it('throws for empty uint8 payloads', () => {
    expect(() => decodeUint8FromBase64(btoa(''))).toThrow('Invalid uint8 payload length');
  });

  describe('parseMetadataBlob', () => {
    it('decodes a multi-entry blob', () => {
      const entries: MetadataBlobEntry[] = [
        { cpfFormat: 0x08, name: 'Brightness (0-1000)' },
        { cpfFormat: 0xe0, name: 'Color' },
        { cpfFormat: 0x01, name: 'Is Active' },
      ];
      expect(parseMetadataBlob(buildMetadataBlobBase64(entries))).toEqual(entries);
    });

    it('decodes a blob with zero entries', () => {
      expect(parseMetadataBlob(buildMetadataBlobBase64([]))).toEqual([]);
    });

    it('returns null for missing input', () => {
      expect(parseMetadataBlob(null)).toBeNull();
      expect(parseMetadataBlob(undefined)).toBeNull();
      expect(parseMetadataBlob('')).toBeNull();
    });

    it('returns null for an unrecognized version byte', () => {
      const entries: MetadataBlobEntry[] = [{ cpfFormat: 0x08, name: 'Step Time Ms' }];
      const blob = buildMetadataBlobBase64(entries, { version: METADATA_BLOB_VERSION + 1 });
      expect(parseMetadataBlob(blob)).toBeNull();
    });

    it('returns null for a payload shorter than the 2-byte header', () => {
      expect(parseMetadataBlob(btoa(String.fromCharCode(METADATA_BLOB_VERSION)))).toBeNull();
    });

    it('returns null when entry_count claims more entries than the payload actually contains', () => {
      const entries: MetadataBlobEntry[] = [{ cpfFormat: 0x08, name: 'Color' }];
      // entry_count says 2 but only one entry's bytes are present.
      const blob = buildMetadataBlobBase64(entries, { entryCount: 2 });
      expect(parseMetadataBlob(blob)).toBeNull();
    });

    it('returns null when a name is truncated before its declared length', () => {
      const entries: MetadataBlobEntry[] = [{ cpfFormat: 0x19, name: 'Animation Name' }];
      // Cut the blob off partway through the name bytes.
      const blob = buildMetadataBlobBase64(entries, { truncateToLength: 5 });
      expect(parseMetadataBlob(blob)).toBeNull();
    });

    it('returns null when truncated immediately after a cpf_format byte (missing name_len)', () => {
      const entries: MetadataBlobEntry[] = [{ cpfFormat: 0x08, name: 'Up Next' }];
      const blob = buildMetadataBlobBase64(entries, { truncateToLength: 3 });
      expect(parseMetadataBlob(blob)).toBeNull();
    });
  });
});
