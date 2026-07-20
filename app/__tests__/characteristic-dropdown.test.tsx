import { fireEvent, render, waitFor } from '@testing-library/react-native';
import React from 'react';

import { CharacteristicDropdown } from '@/components/characteristic-dropdown';
import { CharacteristicInfo } from '@/context/bluetooth-context';
import * as BluetoothContext from '@/context/bluetooth-context';
import { encodeUtf8ToBase64 } from '@/services/ble-value-codec';

function buildCharInfo(value: string): CharacteristicInfo {
  return {
    characteristic: {} as any,
    value,
    name: 'Dropdown Char',
    cpfFormat: 0xe1,
    isUpdateInProgress: false,
  };
}

describe('CharacteristicDropdown', () => {
  beforeEach(() => {
    jest.spyOn(console, 'log').mockImplementation(() => {});
  });

  afterEach(() => {
    jest.restoreAllMocks();
  });

  it('shows the first (selected) option from a \\n-separated value, collapsed by default', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      writeToCharacteristic: jest.fn(async () => true),
    } as any);

    const { getByText, queryByText } = render(
      <CharacteristicDropdown
        charUuid="dropdown-char"
        charInfo={buildCharInfo(encodeUtf8ToBase64('Option B\nOption A\nOption C'))}
      />
    );
    expect(getByText('Option B')).toBeTruthy();
    // The other options aren't shown until the dropdown is opened.
    expect(queryByText('Option A')).toBeNull();
    expect(queryByText('Option C')).toBeNull();
  });

  it('expands in place (no navigation) to show every option when tapped', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      writeToCharacteristic: jest.fn(async () => true),
    } as any);

    const { getByText, getAllByText } = render(
      <CharacteristicDropdown
        charUuid="dropdown-char"
        charInfo={buildCharInfo(encodeUtf8ToBase64('Option B\nOption A\nOption C'))}
      />
    );

    fireEvent.press(getByText('Option B'));

    // "Option B" now appears twice: once as the (still-shown) trigger text, once as its row
    // in the expanded options list.
    expect(getAllByText('Option B')).toHaveLength(2);
    expect(getByText('Option A')).toBeTruthy();
    expect(getByText('Option C')).toBeTruthy();
  });

  it('writes the bare option and optimistically reorders the list selected-first, then collapses', async () => {
    const writeToCharacteristic = jest.fn(async () => true);
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      writeToCharacteristic,
    } as any);

    const { getByText, queryByText } = render(
      <CharacteristicDropdown
        charUuid="dropdown-char"
        charInfo={buildCharInfo(encodeUtf8ToBase64('Option B\nOption A'))}
      />
    );

    fireEvent.press(getByText('Option B'));
    fireEvent.press(getByText('Option A'));

    await waitFor(() => {
      // Bare option is written; the optimistic value is the canonical selected-first list the
      // device will settle on ("Option A\nOption B"), NOT the bare text (which would collapse the
      // list to a single option). This is what makes the UI respond without a notify round-trip.
      expect(writeToCharacteristic).toHaveBeenCalledWith(
        'dropdown-char',
        encodeUtf8ToBase64('Option A'),
        { optimisticValue: encodeUtf8ToBase64('Option A\nOption B') }
      );
    });
    // Collapsed again after picking.
    expect(queryByText('Option A')).toBeNull();
  });

  it('does not write when the already-selected option is tapped again', () => {
    const writeToCharacteristic = jest.fn(async () => true);
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      writeToCharacteristic,
    } as any);

    const { getByText, getAllByText } = render(
      <CharacteristicDropdown
        charUuid="dropdown-char"
        charInfo={buildCharInfo(encodeUtf8ToBase64('Option B\nOption A'))}
      />
    );

    fireEvent.press(getByText('Option B'));
    // Index 1 is the option's row in the expanded list (index 0 is the still-visible trigger).
    fireEvent.press(getAllByText('Option B')[1]);

    expect(writeToCharacteristic).not.toHaveBeenCalled();
  });

  it('renders no selection when the characteristic value is empty', () => {
    jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
      writeToCharacteristic: jest.fn(async () => true),
    } as any);

    const { queryByText } = render(
      <CharacteristicDropdown charUuid="dropdown-char" charInfo={buildCharInfo('')} />
    );
    expect(queryByText(/Option/)).toBeNull();
  });
});
