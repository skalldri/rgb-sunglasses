import React from 'react';
import { Text } from 'react-native';
import { render } from '@testing-library/react-native';

import { getServiceName } from '@/constants/bluetooth';

describe('test setup', () => {
  it('renders a basic React Native component', () => {
    const { getByText } = render(<Text>Test setup works</Text>);
    expect(getByText('Test setup works')).toBeOnTheScreen();
  });

  it('resolves TypeScript path aliases in tests', () => {
    expect(getServiceName('unknown-service-id')).toBe('unknown-service-id');
  });
});
