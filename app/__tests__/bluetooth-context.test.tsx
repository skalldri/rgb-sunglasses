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
});
