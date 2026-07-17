import notifee from '@notifee/react-native';
import { Platform } from 'react-native';

import {
    startConnectionService,
    stopConnectionService,
    updateConnectionNotification,
} from '@/services/ble-foreground-service';

// The module gates every export on Platform.OS at CALL time (the module-scope
// registerForegroundService guard runs at import time and is not re-testable
// here - it's exercised implicitly by the Android build). Platform.OS is a
// writable data property under jest-expo, so replaceProperty flips platforms
// per-test and restoreAllMocks puts it back.

describe('ble-foreground-service', () => {
    beforeEach(() => {
        jest.clearAllMocks();
        jest.spyOn(console, 'log').mockImplementation(() => {});
    });

    afterEach(() => {
        jest.restoreAllMocks();
    });

    describe('on Android', () => {
        beforeEach(() => {
            jest.replaceProperty(Platform, 'OS', 'android');
        });

        it('startConnectionService requests permission, creates the channel, and starts the FGS notification', async () => {
            await startConnectionService('RGB Sunglasses Proto0 94E0');

            expect(notifee.requestPermission).toHaveBeenCalled();
            expect(notifee.createChannel).toHaveBeenCalledWith(
                expect.objectContaining({ id: 'ble-connection' })
            );
            expect(notifee.displayNotification).toHaveBeenCalledWith(
                expect.objectContaining({
                    id: 'ble-connection',
                    body: 'Connected to RGB Sunglasses Proto0 94E0',
                    android: expect.objectContaining({
                        channelId: 'ble-connection',
                        asForegroundService: true,
                        ongoing: true,
                        onlyAlertOnce: true,
                    }),
                })
            );
        });

        it('startConnectionService still starts the service when the permission request fails (non-fatal)', async () => {
            (notifee.requestPermission as jest.Mock).mockRejectedValueOnce(new Error('denied'));

            await startConnectionService('Board');

            expect(notifee.displayNotification).toHaveBeenCalled();
        });

        it('updateConnectionNotification re-displays with new text and never throws', async () => {
            await updateConnectionNotification('Reconnecting to Board…');

            expect(notifee.displayNotification).toHaveBeenCalledWith(
                expect.objectContaining({ id: 'ble-connection', body: 'Reconnecting to Board…' })
            );

            // A failure (e.g. service not running) is swallowed, not thrown.
            (notifee.displayNotification as jest.Mock).mockRejectedValueOnce(new Error('no service'));
            await expect(updateConnectionNotification('x')).resolves.toBeUndefined();
        });

        it('stopConnectionService stops the FGS and cancels the notification', async () => {
            await stopConnectionService();

            expect(notifee.stopForegroundService).toHaveBeenCalled();
            expect(notifee.cancelNotification).toHaveBeenCalledWith('ble-connection');
        });
    });

    describe('on iOS', () => {
        // Platform.OS is already 'ios' under jest-expo's default project; assert the
        // no-op contract explicitly (iOS relies on UIBackgroundModes, not an FGS).
        it('all exports are no-ops', async () => {
            await startConnectionService('Board');
            await updateConnectionNotification('x');
            await stopConnectionService();

            expect(notifee.requestPermission).not.toHaveBeenCalled();
            expect(notifee.createChannel).not.toHaveBeenCalled();
            expect(notifee.displayNotification).not.toHaveBeenCalled();
            expect(notifee.stopForegroundService).not.toHaveBeenCalled();
            expect(notifee.cancelNotification).not.toHaveBeenCalled();
        });
    });
});
