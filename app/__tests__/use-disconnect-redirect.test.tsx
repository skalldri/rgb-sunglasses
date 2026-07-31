import React from 'react';
import { render } from '@testing-library/react-native';

import { useDisconnectRedirect } from '@/hooks/use-disconnect-redirect';
import * as BluetoothContext from '@/context/bluetooth-context';

// Override the global expo-router mock: useFocusEffect must actually run its
// callback (the global mock is a no-op jest.fn()), and useRouter must return a
// single controllable instance. mockFocusState.focused models whether the
// screen owning the hook is the focused one when it mounts.
const mockFocusState = { focused: true };
jest.mock('expo-router', () => {
  // eslint-disable-next-line @typescript-eslint/no-require-imports -- jest.mock factories cannot reference imports
  const ReactActual = require('react');
  return {
    Link: ({ children }: { children: React.ReactNode }) =>
      ReactActual.createElement(ReactActual.Fragment, null, children),
    useRouter: jest.fn(),
    useFocusEffect: (cb: () => void | (() => void)) => {
      ReactActual.useEffect(() => {
        if (!mockFocusState.focused) return undefined;
        return cb();
      }, [cb]);
    },
    useLocalSearchParams: jest.fn(() => ({})),
  };
});

const ExpoRouter = jest.requireMock('expo-router');

function Probe() {
  useDisconnectRedirect();
  return null;
}

function mockDevice(): any {
  return { name: 'RGB Sunglasses', mac: 'AA:BB:CC' };
}

function mockUseBluetooth(selectedDevice: any) {
  jest.spyOn(BluetoothContext, 'useBluetooth').mockReturnValue({
    selectedDevice,
  } as any);
}

describe('useDisconnectRedirect', () => {
  let mockRouter: { navigate: jest.Mock; dismissAll: jest.Mock; canDismiss: jest.Mock };

  beforeEach(() => {
    mockFocusState.focused = true;
    mockRouter = {
      navigate: jest.fn(),
      dismissAll: jest.fn(),
      canDismiss: jest.fn(() => true),
    };
    (ExpoRouter.useRouter as jest.Mock).mockReturnValue(mockRouter);
  });

  afterEach(() => {
    jest.restoreAllMocks();
  });

  it('pops the stack and lands on Connect when the device disconnects while focused', () => {
    mockUseBluetooth(mockDevice());
    const { rerender } = render(<Probe />);
    expect(mockRouter.navigate).not.toHaveBeenCalled();

    mockUseBluetooth(null);
    rerender(<Probe />);

    expect(mockRouter.dismissAll).toHaveBeenCalledTimes(1);
    expect(mockRouter.navigate).toHaveBeenCalledWith('/(tabs)/bluetooth');
  });

  it('skips dismissAll (but still navigates) when there is nothing to dismiss', () => {
    // The Controls index case: top of its stack, canDismiss() is false.
    mockRouter.canDismiss.mockReturnValue(false);
    mockUseBluetooth(mockDevice());
    const { rerender } = render(<Probe />);

    mockUseBluetooth(null);
    rerender(<Probe />);

    expect(mockRouter.dismissAll).not.toHaveBeenCalled();
    expect(mockRouter.navigate).toHaveBeenCalledWith('/(tabs)/bluetooth');
  });

  it('does nothing when mounted already-disconnected at the top of the stack', () => {
    // Visiting the Controls tab while disconnected is a deliberate user action;
    // the index screen's own EmptyState handles it -- no bounce to Connect.
    mockRouter.canDismiss.mockReturnValue(false);
    mockUseBluetooth(null);
    render(<Probe />);

    expect(mockRouter.dismissAll).not.toHaveBeenCalled();
    expect(mockRouter.navigate).not.toHaveBeenCalled();
  });

  it('pops a stale detail screen to the index on focus, without switching tabs', () => {
    // The disconnect happened while this (dismissible) screen was buried;
    // coming back to it pops to the Controls index only.
    mockUseBluetooth(null);
    render(<Probe />);

    expect(mockRouter.dismissAll).toHaveBeenCalledTimes(1);
    expect(mockRouter.navigate).not.toHaveBeenCalled();
  });

  it('does not redirect when the disconnect happens while unfocused', () => {
    mockFocusState.focused = false;
    mockUseBluetooth(mockDevice());
    const { rerender } = render(<Probe />);

    mockUseBluetooth(null);
    rerender(<Probe />);

    expect(mockRouter.dismissAll).not.toHaveBeenCalled();
    expect(mockRouter.navigate).not.toHaveBeenCalled();
  });

  it('does nothing while a device stays connected', () => {
    mockUseBluetooth(mockDevice());
    const { rerender } = render(<Probe />);
    mockUseBluetooth(mockDevice());
    rerender(<Probe />);

    expect(mockRouter.dismissAll).not.toHaveBeenCalled();
    expect(mockRouter.navigate).not.toHaveBeenCalled();
  });
});
