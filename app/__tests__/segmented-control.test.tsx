import { fireEvent, render } from '@testing-library/react-native';
import React from 'react';

import { SegmentedControl } from '@/components/ui/segmented-control';

describe('SegmentedControl', () => {
  beforeEach(() => {
    jest.spyOn(console, 'log').mockImplementation(() => {});
  });

  afterEach(() => {
    jest.restoreAllMocks();
  });

  const options = [
    { label: 'One', value: 1 },
    { label: 'Two', value: 2 },
    { label: 'Three', value: 3 },
  ];

  it('renders every option and marks the current value selected', () => {
    const { getByText, getByRole } = render(
      <SegmentedControl options={options} value={2} onChange={jest.fn()} />
    );
    for (const option of options) {
      expect(getByText(option.label)).toBeTruthy();
    }
    expect(getByRole('button', { name: 'Two', selected: true })).toBeTruthy();
    expect(getByRole('button', { name: 'One', selected: false })).toBeTruthy();
  });

  it('fires onChange with the pressed option value', () => {
    const onChange = jest.fn();
    const { getByText } = render(
      <SegmentedControl options={options} value={1} onChange={onChange} />
    );
    fireEvent.press(getByText('Three'));
    expect(onChange).toHaveBeenCalledWith(3);
  });
});
