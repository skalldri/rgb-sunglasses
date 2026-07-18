import notifee, { AndroidImportance } from '@notifee/react-native';
import { Platform } from 'react-native';

// Android foreground service that keeps the app process - and with it the native
// BLE client holding the GATT connection - alive while the app is backgrounded
// (issue #124). Without it, aggressive OEM background management (OxygenOS
// especially) cache-kills the process, dropping the connection and forcing a
// full re-scan + reconnect. Every export is a no-op on iOS, where UIBackgroundModes
// (bluetooth-central, generated from app.json's ble-plx plugin config) already
// keeps a connected session alive.
//
// Lifecycle contract (enforced by the callers in use-ble-connection.ts):
// - startConnectionService() is only ever called right after a USER-INITIATED
//   connect succeeds - the app is guaranteed foregrounded at that moment, so
//   Android 12+'s ban on starting an FGS from the background can never bite.
// - The service keeps running across auto-reconnect attempts (the notification
//   text is updated, never stop/start - a restart from the background would throw).
// - stopConnectionService() runs on user disconnect / reconnect cancel.
//
// The service type is retyped from notifee's default "shortService" (killed after
// ~3 min on Android 14) to "connectedDevice" by plugins/withBleForegroundService.js.

const CHANNEL_ID = 'ble-connection';
const NOTIFICATION_ID = 'ble-connection';

// notifee requires the long-running task to be registered before the service
// first starts, and registration is app-global - so do it at module load (this
// module is imported by use-ble-connection.ts, which loads with the first screen).
// The task promise deliberately never resolves: the service's lifetime is managed
// explicitly via stopForegroundService(), not by the runner finishing.
if (Platform.OS === 'android') {
    notifee.registerForegroundService(() => new Promise<void>(() => {}));
}

async function displayConnectionNotification(body: string): Promise<void> {
    await notifee.createChannel({
        id: CHANNEL_ID,
        name: 'Device connection',
        importance: AndroidImportance.LOW, // silent, no heads-up - it's a status pin
    });
    await notifee.displayNotification({
        id: NOTIFICATION_ID,
        title: 'RGB Sunglasses',
        body,
        android: {
            channelId: CHANNEL_ID,
            asForegroundService: true,
            ongoing: true,
            // Silent updates: re-displaying with the same id (the Reconnecting…
            // text swap) must not re-alert.
            onlyAlertOnce: true,
            pressAction: { id: 'default' }, // tapping opens the app
        },
    });
}

// Start (or re-purpose, if already running) the foreground service for a freshly
// connected device. POST_NOTIFICATIONS (Android 13+) is requested lazily here;
// denial is non-fatal - the service still runs, the notification is just hidden
// from the shade (Android still surfaces FGS apps in the task manager).
export async function startConnectionService(deviceName: string): Promise<void> {
    if (Platform.OS !== 'android') return;
    try {
        await notifee.requestPermission();
    } catch (error) {
        console.log('Notification permission request failed (non-fatal):', error);
    }
    try {
        await displayConnectionNotification(`Connected to ${deviceName}`);
    } catch (error) {
        console.log('Could not start BLE foreground service:', error);
    }
}

// Update the notification text (e.g. "Reconnecting…") WITHOUT restarting the
// service. Safe to call from the background: updating an existing foreground
// notification is allowed, only *starting* a service is banned there - and if the
// service isn't running (nothing to update), the error is caught and logged.
export async function updateConnectionNotification(body: string): Promise<void> {
    if (Platform.OS !== 'android') return;
    try {
        await displayConnectionNotification(body);
    } catch (error) {
        console.log('Could not update BLE foreground notification:', error);
    }
}

export async function stopConnectionService(): Promise<void> {
    if (Platform.OS !== 'android') return;
    try {
        await notifee.stopForegroundService();
        await notifee.cancelNotification(NOTIFICATION_ID);
    } catch (error) {
        console.log('Could not stop BLE foreground service:', error);
    }
}
