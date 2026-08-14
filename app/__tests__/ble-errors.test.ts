import { CONNECT_FAILED_HINT, describeConnectError, describeWriteError } from '@/services/ble-errors';

describe('describeConnectError', () => {
  it('surfaces the native reason verbatim so a failure is diagnosable from the UI', () => {
    // The board-side signature of the bond mismatch is "Security failed ... err 2"
    // (PIN_OR_KEY_MISSING); app-side ble-plx puts the native stack's text in `reason`.
    expect(describeConnectError({ reason: 'GATT_INSUF_AUTHENTICATION' })).toBe(
      'Could not connect. GATT_INSUF_AUTHENTICATION'
    );
  });

  it('prefers `reason` over `message` (reason is the more specific native text)', () => {
    expect(describeConnectError({ reason: 'status 22', message: 'Operation failed' })).toBe(
      'Could not connect. status 22'
    );
  });

  it('falls back to Error.message for a plain throw', () => {
    expect(describeConnectError(new Error('Device disconnected'))).toBe(
      'Could not connect. Device disconnected'
    );
  });

  it('degrades to the bare message when nothing useful is attached', () => {
    // Must never render "undefined"/"null" at the user - the whole point is that the
    // failure is legible.
    expect(describeConnectError(null)).toBe('Could not connect.');
    expect(describeConnectError(undefined)).toBe('Could not connect.');
    expect(describeConnectError({})).toBe('Could not connect.');
    expect(describeConnectError({ reason: '', message: '' })).toBe('Could not connect.');
  });

  it('names the forget-and-re-pair recovery in the hint', () => {
    // This is the actionable half: a stale bond is the most likely cause and Android
    // will not clear it on its own.
    expect(CONNECT_FAILED_HINT).toMatch(/forget/i);
    expect(CONNECT_FAILED_HINT).toMatch(/bluetooth settings/i);
  });
});

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

  it('maps a reason-only 0xFC/252 status (no numeric code fields) to the friendly refused message', () => {
    expect(describeWriteError({ reason: 'GATT exception ..., status 252 (UNKNOWN), ...' })).toBe(
      'The device refused the change.'
    );
    expect(describeWriteError({ reason: 'operation failed, look up status 0xfc here' })).toBe(
      'The device refused the change.'
    );
  });

  it('does NOT treat an unrelated failure as a refusal (tightened matching, no loose substrings)', () => {
    // An incidental "reject" word without the 0xFC status must not claim the device refused - this
    // is the false-positive the loose fallback used to produce (a disconnect/teardown surfaced on
    // the write).
    expect(describeWriteError({ reason: 'connection rejected by peer', androidErrorCode: 133 })).toBe(
      'Write failed.'
    );
    // "0xfc" only as a substring of a longer hex token must not match \b0xfc\b.
    expect(describeWriteError({ reason: 'write failed with status 0xfcab (other error)' })).toBe(
      'Write failed.'
    );
    // A bare stringified error is not authoritative - only the structured fields/reason are.
    expect(describeWriteError('operation failed (0xfc)')).toBe('Write failed.');
    expect(describeWriteError(new Error('GATT write failed: WRITE_REQ_REJECTED'))).toBe('Write failed.');
  });

  it('falls back to a generic message for unrelated errors', () => {
    expect(describeWriteError(new Error('device disconnected'))).toBe('Write failed.');
    expect(describeWriteError({ attErrorCode: 5, androidErrorCode: 133 })).toBe('Write failed.');
    expect(describeWriteError(null)).toBe('Write failed.');
    expect(describeWriteError(undefined)).toBe('Write failed.');
  });
});
