import '@testing-library/jest-native/extend-expect';
import { Buffer } from 'buffer';

if (!global.atob) {
  global.atob = (data: string) => Buffer.from(data, 'base64').toString('binary');
}

if (!global.btoa) {
  global.btoa = (data: string) => Buffer.from(data, 'binary').toString('base64');
}

jest.mock('react-native-reanimated', () => require('react-native-reanimated/mock'));

jest.mock('expo-router', () => {
  const React = require('react');
  return {
    Link: ({ children }: { children: React.ReactNode }) =>
      React.createElement(React.Fragment, null, children),
    useRouter: () => ({
      navigate: jest.fn(),
      push: jest.fn(),
      replace: jest.fn(),
      back: jest.fn(),
    }),
    useFocusEffect: jest.fn(),
    useLocalSearchParams: jest.fn(() => ({})),
  };
});

jest.mock('expo-document-picker', () => ({
  getDocumentAsync: jest.fn(async () => ({ canceled: true, assets: null })),
}));

jest.mock('expo-file-system/next', () => ({
  File: jest.fn().mockImplementation(() => ({
    base64: jest.fn(async () => ''),
  })),
}));

jest.mock('react-native-ble-plx', () => {
  class BleManager {
    setLogLevel = jest.fn();
    startDeviceScan = jest.fn();
    connectedDevices = jest.fn(async () => []);
    stopDeviceScan = jest.fn();
    connectToDevice = jest.fn();
    cancelDeviceConnection = jest.fn(async () => undefined);
    onDeviceDisconnected = jest.fn(() => ({ remove: jest.fn() }));
  }

  return {
    BleManager,
    LogLevel: {
      Verbose: 'Verbose',
    },
    ConnectionPriority: {
      Balanced: 0,
      High: 1,
      LowPower: 2,
    },
  };
});
