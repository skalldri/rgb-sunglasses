import '@testing-library/jest-native/extend-expect';
import { Buffer } from 'buffer';

if (!global.atob) {
  global.atob = (data: string) => Buffer.from(data, 'base64').toString('binary');
}

if (!global.btoa) {
  global.btoa = (data: string) => Buffer.from(data, 'binary').toString('base64');
}

jest.mock('react-native-reanimated', () => require('react-native-reanimated/mock'));

// Render the gradient as a plain View in tests so kit components (Hero, AppButton,
// ProgressBar) mount without the native module.
jest.mock('expo-linear-gradient', () => {
  const React = require('react');
  const { View } = require('react-native');
  return { LinearGradient: ({ children, ...props }: any) => React.createElement(View, props, children) };
});

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

jest.mock('expo-file-system/legacy', () => ({
  cacheDirectory: 'file:///cache/',
  createDownloadResumable: jest.fn(() => ({
    downloadAsync: jest.fn(async () => ({ uri: 'file:///cache/firmware-update.zip', status: 200 })),
  })),
  getContentUriAsync: jest.fn(async (uri: string) => `content://mock/${uri}`),
}));

jest.mock('expo-intent-launcher', () => ({
  startActivityAsync: jest.fn(async () => ({ resultCode: -1 })),
}));

jest.mock('expo-web-browser', () => ({
  openBrowserAsync: jest.fn(async () => ({ type: 'opened' })),
}));

jest.mock('@notifee/react-native', () => ({
  __esModule: true,
  default: {
    registerForegroundService: jest.fn(),
    createChannel: jest.fn(async () => 'ble-connection'),
    displayNotification: jest.fn(async () => 'ble-connection'),
    stopForegroundService: jest.fn(async () => undefined),
    cancelNotification: jest.fn(async () => undefined),
    requestPermission: jest.fn(async () => ({ authorizationStatus: 1 })),
  },
  AndroidImportance: {
    NONE: 0,
    MIN: 1,
    LOW: 2,
    DEFAULT: 3,
    HIGH: 4,
  },
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
    ScanMode: {
      Opportunistic: -1,
      LowPower: 0,
      Balanced: 1,
      LowLatency: 2,
    },
  };
});
