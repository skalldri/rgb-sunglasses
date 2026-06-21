import {
  decodeBooleanFromBase64,
  decodeColorFromBase64,
  decodeFloat32FromBase64,
  decodeUint32FromBase64,
  decodeUtf8FromBase64,
  encodeBooleanToBase64,
  encodeColorToBase64,
  encodeFloat32ToBase64,
  encodeUint32ToBase64,
  encodeUtf8ToBase64,
  sanitizeFloatInput,
  sanitizeNumericInput,
} from '@/services/ble-value-codec';

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
});
