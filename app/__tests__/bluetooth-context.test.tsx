import React from 'react';
import { act, render, waitFor } from '@testing-library/react-native';

import { BLE_GATT_CPF_FORMAT_UTF8S } from '@/constants/bluetooth';
import { BluetoothProvider, CharacteristicInfo, useBluetooth } from '@/context/bluetooth-context';

type BluetoothApi = ReturnType<typeof useBluetooth>;

function setupProvider() {
  let api: BluetoothApi | null = null;

  function CaptureApi() {
    api = useBluetooth();
    return null;
  }

  render(
    <BluetoothProvider>
      <CaptureApi />
    </BluetoothProvider>
  );

  return () => {
    if (!api) {
      throw new Error('Bluetooth API not available');
    }
    return api;
  };
}

function buildSelectedDevice(writeWithResponse: jest.Mock, value: string = btoa('old')) {
  const charInfo: CharacteristicInfo = {
    characteristic: {
      writeWithResponse,
    } as any,
    value,
    name: 'Label',
    cpfFormat: BLE_GATT_CPF_FORMAT_UTF8S,
    isUpdateInProgress: false,
  };

  return {
    name: 'Device',
    mac: 'AA:BB:CC',
    device: {} as any,
    services: [{ uuid: 'service-1' }] as any,
    characteristicsByService: {
      'service-1': {
        'char-1': charInfo,
      },
    },
    characteristics: {
      'char-1': charInfo,
    },
    serviceCharacteristics: {
      'service-1': ['char-1'],
    },
  };
}

describe('BluetoothProvider', () => {
  beforeEach(() => {
    jest.spyOn(console, 'log').mockImplementation(() => {});
  });

  afterEach(() => {
    jest.restoreAllMocks();
  });

  it('returns false when writing to a missing characteristic', async () => {
    const getApi = setupProvider();
    const writeWithResponse = jest.fn(async () => undefined);

    act(() => {
      getApi().setSelectedDevice(buildSelectedDevice(writeWithResponse));
    });

    let result = true;
    await act(async () => {
      result = await getApi().writeToCharacteristic('missing-char', btoa('new'));
    });
    expect(result).toBe(false);
    expect(writeWithResponse).not.toHaveBeenCalled();
  });

  it('toggles isUpdateInProgress and updates value on successful writes', async () => {
    const getApi = setupProvider();
    let resolveWrite!: () => void;
    const deferredWrite = new Promise<void>((resolve) => {
      resolveWrite = resolve;
    });
    const writeWithResponse = jest.fn(() => deferredWrite);

    act(() => {
      getApi().setSelectedDevice(buildSelectedDevice(writeWithResponse));
    });

    let writePromise!: Promise<boolean>;
    act(() => {
      writePromise = getApi().writeToCharacteristic('char-1', btoa('new'));
    });

    await waitFor(() => {
      expect(getApi().getCharacteristicInfo('char-1')?.isUpdateInProgress).toBe(true);
    });

    let result = false;
    await act(async () => {
      resolveWrite();
      result = await writePromise;
    });
    expect(result).toBe(true);

    await waitFor(() => {
      const charInfo = getApi().getCharacteristicInfo('char-1');
      expect(charInfo?.isUpdateInProgress).toBe(false);
      expect(charInfo?.value).toBe(btoa('new'));
    });
  });

  // Regression test for issue #91 (controlled Switch flicker): the optimistic value must be applied
  // synchronously alongside isUpdateInProgress=true, BEFORE the BLE write resolves, so a controlled
  // input never renders with its stale value mid-write.
  it('applies the optimistic value before the write resolves', async () => {
    const getApi = setupProvider();
    let resolveWrite!: () => void;
    const deferredWrite = new Promise<void>((resolve) => {
      resolveWrite = resolve;
    });
    const writeWithResponse = jest.fn(() => deferredWrite);

    act(() => {
      getApi().setSelectedDevice(buildSelectedDevice(writeWithResponse));
    });

    let writePromise!: Promise<boolean>;
    act(() => {
      writePromise = getApi().writeToCharacteristic('char-1', btoa('new'));
    });

    // While the write is still in flight, the value is ALREADY the new value (not the old one) and
    // the update-in-progress flag is set — both from the same synchronous, pre-await render.
    await waitFor(() => {
      const charInfo = getApi().getCharacteristicInfo('char-1');
      expect(charInfo?.isUpdateInProgress).toBe(true);
      expect(charInfo?.value).toBe(btoa('new'));
    });

    await act(async () => {
      resolveWrite();
      await writePromise;
    });
  });

  it('toggles isUpdateInProgress and preserves previous value on failed writes', async () => {
    const getApi = setupProvider();
    const writeWithResponse = jest.fn(async () => {
      throw new Error('write failed');
    });

    act(() => {
      getApi().setSelectedDevice(buildSelectedDevice(writeWithResponse, btoa('stable')));
    });

    let result = true;
    await act(async () => {
      result = await getApi().writeToCharacteristic('char-1', btoa('new'));
    });
    expect(result).toBe(false);

    await waitFor(() => {
      const charInfo = getApi().getCharacteristicInfo('char-1');
      expect(charInfo?.isUpdateInProgress).toBe(false);
      expect(charInfo?.value).toBe(btoa('stable'));
    });
  });

  // A device notification (or overlapping write) can update the value while a write is in flight.
  // If that write later fails, the compare-and-swap revert must NOT clobber the fresher value.
  it('does not revert a value changed mid-write when the write later fails', async () => {
    const getApi = setupProvider();
    let rejectWrite!: (e: Error) => void;
    const deferredWrite = new Promise<void>((_resolve, reject) => {
      rejectWrite = reject;
    });
    deferredWrite.catch(() => {}); // pre-attach so the pending rejection is never "unhandled"
    const writeWithResponse = jest.fn(() => deferredWrite);

    act(() => {
      getApi().setSelectedDevice(buildSelectedDevice(writeWithResponse, btoa('old')));
    });

    let writePromise!: Promise<boolean>;
    act(() => {
      writePromise = getApi().writeToCharacteristic('char-1', btoa('new'));
    });

    // Device notification lands mid-write carrying the device's real value.
    act(() => {
      getApi().updateCharValue('char-1', btoa('device'));
    });

    let result = true;
    await act(async () => {
      rejectWrite(new Error('write failed'));
      result = await writePromise;
    });
    expect(result).toBe(false);

    await waitFor(() => {
      const charInfo = getApi().getCharacteristicInfo('char-1');
      expect(charInfo?.isUpdateInProgress).toBe(false);
      // The mid-write notification value survives; it is NOT clobbered back to 'old'.
      expect(charInfo?.value).toBe(btoa('device'));
    });
  });

  it('skips the optimistic value update when skipOptimisticUpdate is set, without affecting success/progress reporting', async () => {
    const getApi = setupProvider();
    const writeWithResponse = jest.fn(async () => undefined);

    act(() => {
      getApi().setSelectedDevice(buildSelectedDevice(writeWithResponse, btoa('stable')));
    });

    let result = false;
    await act(async () => {
      result = await getApi().writeToCharacteristic('char-1', btoa('written'), { skipOptimisticUpdate: true });
    });
    expect(result).toBe(true);
    expect(writeWithResponse).toHaveBeenCalledWith(btoa('written'));

    await waitFor(() => {
      const charInfo = getApi().getCharacteristicInfo('char-1');
      expect(charInfo?.isUpdateInProgress).toBe(false);
      // Local value is untouched: a real device (e.g. one backing a dropdown-list
      // characteristic) may store/notify something other than the bare written bytes.
      expect(charInfo?.value).toBe(btoa('stable'));
    });
  });

  it('updateCharValue updates mapped characteristic values', async () => {
    const getApi = setupProvider();
    const writeWithResponse = jest.fn(async () => undefined);

    act(() => {
      getApi().setSelectedDevice(buildSelectedDevice(writeWithResponse, btoa('before')));
    });

    act(() => {
      getApi().updateCharValue('char-1', btoa('after'));
    });

    await waitFor(() => {
      expect(getApi().getCharacteristicInfo('char-1')?.value).toBe(btoa('after'));
    });
  });

  // Service-aware methods exist for characteristics whose UUID is intentionally reused across
  // multiple services (e.g. UUID_IS_ACTIVE_CHARACTERISTIC) - the bare-UUID flat map can't
  // disambiguate those, so these resolve/patch via characteristicsByService[serviceUuid] instead.
  describe('service-aware characteristic methods', () => {
    function buildDeviceWithDuplicatedCharUuid(writeWithResponseA: jest.Mock, writeWithResponseB: jest.Mock) {
      const charInfoA: CharacteristicInfo = {
        characteristic: { writeWithResponse: writeWithResponseA } as any,
        value: btoa('a-old'),
        name: 'Is Active',
        cpfFormat: 0x01,
        isUpdateInProgress: false,
      };
      const charInfoB: CharacteristicInfo = {
        characteristic: { writeWithResponse: writeWithResponseB } as any,
        value: btoa('b-old'),
        name: 'Is Active',
        cpfFormat: 0x01,
        isUpdateInProgress: false,
      };

      return {
        name: 'Device',
        mac: 'AA:BB:CC',
        device: {} as any,
        services: [{ uuid: 'service-a' }, { uuid: 'service-b' }] as any,
        characteristicsByService: {
          'service-a': { 'dup-char': charInfoA },
          'service-b': { 'dup-char': charInfoB },
        },
        // Intentionally empty: a duplicated UUID can't be represented unambiguously here.
        characteristics: {},
        serviceCharacteristics: { 'service-a': [], 'service-b': [] },
      };
    }

    it('getServiceCharacteristicInfo resolves the correct service-scoped entry, not a flat-map lookup', () => {
      const getApi = setupProvider();
      const writeA = jest.fn(async () => undefined);
      const writeB = jest.fn(async () => undefined);

      act(() => {
        getApi().setSelectedDevice(buildDeviceWithDuplicatedCharUuid(writeA, writeB));
      });

      expect(getApi().getServiceCharacteristicInfo('service-a', 'dup-char')?.value).toBe(btoa('a-old'));
      expect(getApi().getServiceCharacteristicInfo('service-b', 'dup-char')?.value).toBe(btoa('b-old'));
      // The flat map can't hold this UUID at all.
      expect(getApi().getCharacteristicInfo('dup-char')).toBeNull();
    });

    it('writeServiceCharacteristic writes via the correct service\'s characteristic object and patches only that service', async () => {
      const getApi = setupProvider();
      const writeA = jest.fn(async () => undefined);
      const writeB = jest.fn(async () => undefined);

      act(() => {
        getApi().setSelectedDevice(buildDeviceWithDuplicatedCharUuid(writeA, writeB));
      });

      let result = false;
      await act(async () => {
        result = await getApi().writeServiceCharacteristic('service-a', 'dup-char', btoa('a-new'));
      });

      expect(result).toBe(true);
      expect(writeA).toHaveBeenCalledWith(btoa('a-new'));
      expect(writeB).not.toHaveBeenCalled();

      await waitFor(() => {
        expect(getApi().getServiceCharacteristicInfo('service-a', 'dup-char')?.value).toBe(btoa('a-new'));
        // service-b's entry for the same UUID is untouched.
        expect(getApi().getServiceCharacteristicInfo('service-b', 'dup-char')?.value).toBe(btoa('b-old'));
      });
    });

    // Regression test for issue #91: the "Is Active" toggle is the flat-map-excluded, service-scoped
    // characteristic that actually flickered. Its optimistic value must land before the write
    // resolves, same as the bare-UUID path.
    it('writeServiceCharacteristic applies the optimistic value before the write resolves', async () => {
      const getApi = setupProvider();
      let resolveWrite!: () => void;
      const deferredWrite = new Promise<void>((resolve) => {
        resolveWrite = resolve;
      });
      const writeA = jest.fn(() => deferredWrite);
      const writeB = jest.fn(async () => undefined);

      act(() => {
        getApi().setSelectedDevice(buildDeviceWithDuplicatedCharUuid(writeA, writeB));
      });

      let writePromise!: Promise<boolean>;
      act(() => {
        writePromise = getApi().writeServiceCharacteristic('service-a', 'dup-char', btoa('a-new'));
      });

      await waitFor(() => {
        const charInfo = getApi().getServiceCharacteristicInfo('service-a', 'dup-char');
        expect(charInfo?.isUpdateInProgress).toBe(true);
        expect(charInfo?.value).toBe(btoa('a-new'));
      });
      // The sibling service's identically-UUID'd entry is untouched throughout.
      expect(getApi().getServiceCharacteristicInfo('service-b', 'dup-char')?.value).toBe(btoa('b-old'));

      await act(async () => {
        resolveWrite();
        await writePromise;
      });
    });

    it('writeServiceCharacteristic reverts the optimistic value when the write is rejected', async () => {
      const getApi = setupProvider();
      const writeA = jest.fn(async () => {
        throw new Error('write failed');
      });
      const writeB = jest.fn(async () => undefined);

      act(() => {
        getApi().setSelectedDevice(buildDeviceWithDuplicatedCharUuid(writeA, writeB));
      });

      let result = true;
      await act(async () => {
        result = await getApi().writeServiceCharacteristic('service-a', 'dup-char', btoa('a-new'));
      });
      expect(result).toBe(false);

      await waitFor(() => {
        const charInfo = getApi().getServiceCharacteristicInfo('service-a', 'dup-char');
        expect(charInfo?.isUpdateInProgress).toBe(false);
        expect(charInfo?.value).toBe(btoa('a-old'));
      });
    });

    it('writeServiceCharacteristic keeps a mid-write notification value when the write later fails', async () => {
      const getApi = setupProvider();
      let rejectWrite!: (e: Error) => void;
      const deferredWrite = new Promise<void>((_resolve, reject) => {
        rejectWrite = reject;
      });
      deferredWrite.catch(() => {});
      const writeA = jest.fn(() => deferredWrite);
      const writeB = jest.fn(async () => undefined);

      act(() => {
        getApi().setSelectedDevice(buildDeviceWithDuplicatedCharUuid(writeA, writeB));
      });

      let writePromise!: Promise<boolean>;
      act(() => {
        writePromise = getApi().writeServiceCharacteristic('service-a', 'dup-char', btoa('a-new'));
      });

      // A notification for service-a's "Is Active" lands mid-write with the device's real value.
      act(() => {
        getApi().updateServiceCharacteristicValue('service-a', 'dup-char', btoa('a-device'));
      });

      let result = true;
      await act(async () => {
        rejectWrite(new Error('write failed'));
        result = await writePromise;
      });
      expect(result).toBe(false);

      await waitFor(() => {
        // Notified value survives the failed write; not reverted to 'a-old'.
        expect(getApi().getServiceCharacteristicInfo('service-a', 'dup-char')?.value).toBe(btoa('a-device'));
      });
    });

    it('writeServiceCharacteristic returns false for a missing service/characteristic pair', async () => {
      const getApi = setupProvider();
      const writeA = jest.fn(async () => undefined);
      const writeB = jest.fn(async () => undefined);

      act(() => {
        getApi().setSelectedDevice(buildDeviceWithDuplicatedCharUuid(writeA, writeB));
      });

      let result = true;
      await act(async () => {
        result = await getApi().writeServiceCharacteristic('service-a', 'missing-char', btoa('x'));
      });
      expect(result).toBe(false);
    });

    it('updateServiceCharacteristicValue patches only the targeted service entry', async () => {
      const getApi = setupProvider();
      const writeA = jest.fn(async () => undefined);
      const writeB = jest.fn(async () => undefined);

      act(() => {
        getApi().setSelectedDevice(buildDeviceWithDuplicatedCharUuid(writeA, writeB));
      });

      act(() => {
        getApi().updateServiceCharacteristicValue('service-b', 'dup-char', btoa('b-new'));
      });

      await waitFor(() => {
        expect(getApi().getServiceCharacteristicInfo('service-b', 'dup-char')?.value).toBe(btoa('b-new'));
        expect(getApi().getServiceCharacteristicInfo('service-a', 'dup-char')?.value).toBe(btoa('a-old'));
      });
    });
  });
});
