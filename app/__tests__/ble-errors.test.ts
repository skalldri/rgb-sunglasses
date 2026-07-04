import { describeWriteError } from '@/services/ble-errors';

describe('describeWriteError', () => {
  it('maps an ATT WRITE_REQ_REJECTED code (0xFC) in attErrorCode (iOS shape) to the friendly refused message', () => {
    expect(describeWriteError({ attErrorCode: 0xfc })).toBe('The device refused the change.');
  });

  it('maps androidErrorCode 252 (0xFC) to the friendly refused message (real hardware-verified Android shape, issue #92)', () => {
    // On Android a refused write arrives as androidErrorCode=252 with attErrorCode=null and a
    // reason string that mentions "status 252"/"0xfc" but never the word "reject".
    expect(
      describeWriteError({
        errorCode: 401,
        attErrorCode: null,
        androidErrorCode: 252,
        iosErrorCode: null,
        reason:
          "GATT exception from MAC='..', status 252 (UNKNOWN), type BleGattOperation{description='CHARACTERISTIC_WRITE'}. (Look up status 0xfc here ...)",
        message: 'Characteristic .. write failed for device ..',
      })
    ).toBe('The device refused the change.');
  });

  it('maps a "reject" reason string to the friendly refused message', () => {
    expect(describeWriteError({ reason: 'Write request rejected' })).toBe('The device refused the change.');
  });

  it('matches a rejected error surfaced only in the stringified form', () => {
    expect(describeWriteError(new Error('GATT write failed: WRITE_REQ_REJECTED'))).toBe(
      'The device refused the change.'
    );
    // Bare ATT code embedded in the message.
    expect(describeWriteError('operation failed (0xfc)')).toBe('The device refused the change.');
  });

  it('falls back to a generic message for unrelated errors', () => {
    expect(describeWriteError(new Error('device disconnected'))).toBe('Write failed.');
    expect(describeWriteError({ attErrorCode: 5, androidErrorCode: 133 })).toBe('Write failed.');
    expect(describeWriteError(null)).toBe('Write failed.');
    expect(describeWriteError(undefined)).toBe('Write failed.');
  });
});
