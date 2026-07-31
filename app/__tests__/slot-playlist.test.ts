import {
  BLE_GATT_CPF_FORMAT_BOOLEAN,
  BLE_GATT_CPF_FORMAT_SLOT_NOW_PLAYING,
  BLE_GATT_CPF_FORMAT_SLOT_TEXT,
  BLE_GATT_CPF_FORMAT_SLOT_UP_NEXT,
  BLE_GATT_CPF_FORMAT_UINT32,
  BLE_GATT_CPF_FORMAT_UTF8S,
  UUID_IS_ACTIVE_CHARACTERISTIC,
} from '@/constants/bluetooth';
import { encodeBooleanToBase64, encodeUint32ToBase64, encodeUtf8ToBase64 } from '@/services/ble-value-codec';
import { decodeSlotIndex, groupSlotPlaylist, isServiceActive } from '@/services/slot-playlist';

function char(cpfFormat: number, value: string | null = null) {
  return {
    characteristic: {} as any,
    value,
    name: null,
    cpfFormat,
    isUpdateInProgress: false,
  };
}

describe('groupSlotPlaylist', () => {
  it('indexes slots by appearance order even when interleaved with other formats', () => {
    // Models the real Text service layout: params first, slots, then Now Playing AFTER
    // other characteristics (auto id 23 sits past Animation Name in firmware) — grouping
    // must be by CPF format, not adjacency.
    const chars = {
      'step-time': char(BLE_GATT_CPF_FORMAT_UINT32),
      'up-next': char(BLE_GATT_CPF_FORMAT_SLOT_UP_NEXT, encodeUint32ToBase64(1)),
      'slot-a': char(BLE_GATT_CPF_FORMAT_SLOT_TEXT, encodeUtf8ToBase64('A')),
      'slot-b': char(BLE_GATT_CPF_FORMAT_SLOT_TEXT, encodeUtf8ToBase64('B')),
      'other-text': char(BLE_GATT_CPF_FORMAT_UTF8S),
      'slot-c': char(BLE_GATT_CPF_FORMAT_SLOT_TEXT, encodeUtf8ToBase64('C')),
      'now-playing': char(BLE_GATT_CPF_FORMAT_SLOT_NOW_PLAYING, encodeUint32ToBase64(0)),
    };

    const playlist = groupSlotPlaylist(chars as any);
    expect(playlist).not.toBeNull();
    expect(playlist!.slots.map(s => s.charUuid)).toEqual(['slot-a', 'slot-b', 'slot-c']);
    expect(playlist!.slots.map(s => s.slotIndex)).toEqual([0, 1, 2]);
    expect(playlist!.upNext?.charUuid).toBe('up-next');
    expect(playlist!.nowPlaying?.charUuid).toBe('now-playing');
    // Non-slot characteristics stay visible.
    expect(playlist!.hiddenCharUuids).toEqual(
      new Set(['slot-a', 'slot-b', 'slot-c', 'up-next', 'now-playing']),
    );
  });

  it('returns null when the service has no SLOT_TEXT characteristics', () => {
    expect(groupSlotPlaylist({ 'plain': char(BLE_GATT_CPF_FORMAT_UINT32) } as any)).toBeNull();
  });

  it('returns null for a stray SLOT_UP_NEXT with zero slots (degraded standalone fallback)', () => {
    const chars = { 'up-next': char(BLE_GATT_CPF_FORMAT_SLOT_UP_NEXT, encodeUint32ToBase64(0)) };
    expect(groupSlotPlaylist(chars as any)).toBeNull();
  });

  it('tolerates a missing upNext / nowPlaying', () => {
    const playlist = groupSlotPlaylist({
      'slot-a': char(BLE_GATT_CPF_FORMAT_SLOT_TEXT),
    } as any);
    expect(playlist).not.toBeNull();
    expect(playlist!.upNext).toBeNull();
    expect(playlist!.nowPlaying).toBeNull();
    expect(playlist!.hiddenCharUuids).toEqual(new Set(['slot-a']));
  });

  it('keeps only the first of duplicate up-next characteristics; extras stay visible', () => {
    const playlist = groupSlotPlaylist({
      'slot-a': char(BLE_GATT_CPF_FORMAT_SLOT_TEXT),
      'up-next-1': char(BLE_GATT_CPF_FORMAT_SLOT_UP_NEXT),
      'up-next-2': char(BLE_GATT_CPF_FORMAT_SLOT_UP_NEXT),
    } as any);
    expect(playlist!.upNext?.charUuid).toBe('up-next-1');
    expect(playlist!.hiddenCharUuids.has('up-next-2')).toBe(false);
  });
});

describe('isServiceActive', () => {
  it('reads the shared-UUID Is Active characteristic', () => {
    expect(isServiceActive({
      [UUID_IS_ACTIVE_CHARACTERISTIC]: char(BLE_GATT_CPF_FORMAT_BOOLEAN, encodeBooleanToBase64(true)),
    } as any)).toBe(true);
    expect(isServiceActive({
      [UUID_IS_ACTIVE_CHARACTERISTIC]: char(BLE_GATT_CPF_FORMAT_BOOLEAN, encodeBooleanToBase64(false)),
    } as any)).toBe(false);
  });

  it('reads as inactive when the characteristic or its value is missing', () => {
    expect(isServiceActive({} as any)).toBe(false);
    expect(isServiceActive({
      [UUID_IS_ACTIVE_CHARACTERISTIC]: char(BLE_GATT_CPF_FORMAT_BOOLEAN, null),
    } as any)).toBe(false);
  });
});

describe('decodeSlotIndex', () => {
  it('decodes a uint32 LE value', () => {
    expect(decodeSlotIndex(char(BLE_GATT_CPF_FORMAT_SLOT_UP_NEXT, encodeUint32ToBase64(7)) as any)).toBe(7);
  });

  it('returns null for a missing charInfo or value', () => {
    expect(decodeSlotIndex(null)).toBeNull();
    expect(decodeSlotIndex(undefined)).toBeNull();
    expect(decodeSlotIndex(char(BLE_GATT_CPF_FORMAT_SLOT_UP_NEXT, null) as any)).toBeNull();
  });

  it('returns null for a short/garbage payload', () => {
    expect(decodeSlotIndex(char(BLE_GATT_CPF_FORMAT_SLOT_UP_NEXT, btoa('\x01')) as any)).toBeNull();
  });
});
