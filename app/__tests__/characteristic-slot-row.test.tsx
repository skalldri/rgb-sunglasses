import { fireEvent, render } from '@testing-library/react-native';
import React from 'react';
import { Text } from 'react-native';

import { CharacteristicSlotRow } from '@/components/characteristic-slot-row';

function renderRow(overrides: Partial<React.ComponentProps<typeof CharacteristicSlotRow>> = {}) {
  const onQueueUpNext = jest.fn();
  const utils = render(
    <CharacteristicSlotRow
      label="Slot 3"
      slotIndex={3}
      isNowPlaying={false}
      isUpNext={false}
      showUpNextButton={true}
      upNextDisabled={false}
      onQueueUpNext={onQueueUpNext}
      {...overrides}
    >
      <Text>child-input</Text>
    </CharacteristicSlotRow>,
  );
  return { onQueueUpNext, ...utils };
}

describe('CharacteristicSlotRow', () => {
  it('renders the label, children, and the up-next button', () => {
    const { getByText, getByTestId } = renderRow();
    expect(getByText('Slot 3')).toBeTruthy();
    expect(getByText('child-input')).toBeTruthy();
    expect(getByTestId('slot-up-next-3')).toBeTruthy();
  });

  it('omits the up-next button when the service has no up-next characteristic', () => {
    const { queryByTestId, getByText } = renderRow({ showUpNextButton: false });
    expect(queryByTestId('slot-up-next-3')).toBeNull();
    expect(getByText('child-input')).toBeTruthy();
  });

  it('fires onQueueUpNext once per press', () => {
    const { getByTestId, onQueueUpNext } = renderRow();
    fireEvent.press(getByTestId('slot-up-next-3'));
    expect(onQueueUpNext).toHaveBeenCalledTimes(1);
  });

  it('marks the button selected when this slot is up next', () => {
    const { getByTestId } = renderRow({ isUpNext: true });
    expect(getByTestId('slot-up-next-3').props.accessibilityState.selected).toBe(true);
  });

  it('is muted/unselected when neither up next nor now playing', () => {
    const { getByTestId, queryByLabelText } = renderRow();
    expect(getByTestId('slot-up-next-3').props.accessibilityState.selected).toBe(false);
    expect(queryByLabelText('Slot 3, now playing')).toBeNull();
  });

  it('renders the now-playing treatment with a non-color cue', () => {
    const { getByLabelText } = renderRow({ isNowPlaying: true });
    // The row-level accessibility label carries the now-playing state (never color alone).
    expect(getByLabelText('Slot 3, now playing')).toBeTruthy();
  });

  it('composes both treatments when the slot is playing AND queued to repeat', () => {
    const { getByLabelText, getByTestId } = renderRow({ isNowPlaying: true, isUpNext: true });
    expect(getByLabelText('Slot 3, now playing')).toBeTruthy();
    expect(getByTestId('slot-up-next-3').props.accessibilityState.selected).toBe(true);
  });

  it('disables the button (no press, disabled state) while an up-next write is in flight', () => {
    const { getByTestId, onQueueUpNext } = renderRow({ upNextDisabled: true });
    const button = getByTestId('slot-up-next-3');
    expect(button.props.accessibilityState.disabled).toBe(true);
    fireEvent.press(button);
    expect(onQueueUpNext).not.toHaveBeenCalled();
  });
});
