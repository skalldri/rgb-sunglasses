const { withAndroidManifest } = require('@expo/config-plugins');

// Retype notifee's foreground service for long-lived BLE sessions (issue #124).
//
// @notifee/react-native's bundled core AAR declares
//   <service android:name="app.notifee.core.ForegroundService"
//            android:foregroundServiceType="shortService" />
// (verified by unzipping node_modules/@notifee/react-native/android/libs/.../core-*.aar).
// "shortService" is hard-limited to ~3 minutes on Android 14+ - useless for keeping a
// BLE connection alive through a concert. Override it at manifest-merge time to
// "connectedDevice" (the typed FGS for interacting with a connected external device,
// exactly our use case; requires the FOREGROUND_SERVICE_CONNECTED_DEVICE permission,
// declared in app.json). tools:replace makes our attribute win over the library's.
module.exports = function withBleForegroundService(config) {
  return withAndroidManifest(config, (cfg) => {
    const manifest = cfg.modResults.manifest;
    manifest.$['xmlns:tools'] = 'http://schemas.android.com/tools';

    const application = manifest.application?.[0];
    if (!application) return cfg;

    if (!application.service) application.service = [];
    let service = application.service.find(
      (s) => s.$?.['android:name'] === 'app.notifee.core.ForegroundService'
    );
    if (!service) {
      service = { $: { 'android:name': 'app.notifee.core.ForegroundService' } };
      application.service.push(service);
    }
    service.$['android:foregroundServiceType'] = 'connectedDevice';
    service.$['tools:replace'] = 'android:foregroundServiceType';

    return cfg;
  });
};
