import { PermissionsAndroid, Platform } from 'react-native';

import { requestPermissions } from '@/hooks/ble-manager';

// requestPermissions branches on ExpoDevice.platformApiLevel; expose it as a
// mutable mock so each test can pick the API level. The jest.mock factory runs
// during hoisted-import evaluation - BEFORE this module's own initializers - so
// the factory must not capture a value eagerly (a plain `const mockDevice =
// {...}` is still undefined at that point); the getter defers the read to call
// time. (`mock` prefix required for outer bindings referenced in the factory.)
let mockPlatformApiLevel = 34;
jest.mock('expo-device', () => ({
    get platformApiLevel() {
        return mockPlatformApiLevel;
    },
}));

describe('requestPermissions', () => {
    let requestSpy: jest.SpyInstance;

    beforeEach(() => {
        jest.clearAllMocks();
        requestSpy = jest
            .spyOn(PermissionsAndroid, 'request')
            .mockResolvedValue('granted' as never);
    });

    afterEach(() => {
        jest.restoreAllMocks();
    });

    describe('on Android API 31+', () => {
        beforeEach(() => {
            jest.replaceProperty(Platform, 'OS', 'android');
            mockPlatformApiLevel = 34;
        });

        it('requests only BLUETOOTH_SCAN and BLUETOOTH_CONNECT - never ACCESS_FINE_LOCATION (neverForLocation, issue #189)', async () => {
            const granted = await requestPermissions();

            expect(granted).toBe(true);
            const requested = requestSpy.mock.calls.map(call => call[0]);
            expect(requested).toContain(PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN);
            expect(requested).toContain(PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT);
            // The manifest caps ACCESS_FINE_LOCATION at maxSdkVersion 30, so a
            // request here would always come back denied and block scanning.
            expect(requested).not.toContain(PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION);
        });

        it('returns false when a Bluetooth permission is denied', async () => {
            requestSpy.mockResolvedValueOnce('granted' as never).mockResolvedValueOnce('denied' as never);

            expect(await requestPermissions()).toBe(false);
        });
    });

    describe('on Android API < 31', () => {
        beforeEach(() => {
            jest.replaceProperty(Platform, 'OS', 'android');
            mockPlatformApiLevel = 29;
        });

        it('still requests ACCESS_FINE_LOCATION (required for BLE scanning pre-31)', async () => {
            const granted = await requestPermissions();

            expect(granted).toBe(true);
            expect(requestSpy).toHaveBeenCalledWith(
                PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION,
                expect.anything()
            );
        });
    });

    describe('on iOS', () => {
        it('resolves true without requesting anything', async () => {
            jest.replaceProperty(Platform, 'OS', 'ios');

            expect(await requestPermissions()).toBe(true);
            expect(requestSpy).not.toHaveBeenCalled();
        });
    });
});
