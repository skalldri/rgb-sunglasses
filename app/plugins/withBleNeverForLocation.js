const { withAndroidManifest, AndroidConfig } = require('@expo/config-plugins');

// react-native-ble-plx's own Expo plugin (withBLEAndroidManifest.js, via
// addLocationPermissionToManifest / addScanPermissionToManifest) only ADDS the
// BLUETOOTH_SCAN / ACCESS_*_LOCATION manifest entries when they are entirely
// absent - it never retrofits `android:usesPermissionFlags="neverForLocation"` or
// `android:maxSdkVersion="30"` onto entries that already exist. That is fatal in
// this repo specifically because `app/scripts/launch-app.sh` runs
// `expo prebuild` WITHOUT `--clean` on every launch: once an `android/` checkout
// has those entries from an earlier prebuild (e.g. before `neverForLocation` was
// set, or from any prebuild that predates this plugin), ble-plx's "add if absent"
// guard short-circuits forever and the flags can never land - even though
// `app.json` correctly asks for `neverForLocation: true`.
//
// Consequence when this goes wrong: on Android 12+ (API 31+) the manifest
// declares BLUETOOTH_SCAN without `neverForLocation`, so the OS requires a
// location permission for scanning to return results - but
// `requestAndroid31Permissions()` in `hooks/ble-manager.ts` deliberately never
// requests one (see the comment there), because it assumes the flag is present.
// Scanning then silently returns nothing unless location happens to be granted
// through some other path. On OxygenOS, `pm grant` is blocked, so there is not
// even a manual ADB workaround.
//
// Fix: run AFTER "react-native-ble-plx" in app.json's plugin list and ENSURE the
// correct attributes on the existing entries (creating them if missing too, so
// this also works from a clean prebuild). Idempotent - safe to run on every
// prebuild, clean or incremental.
module.exports = function withBleNeverForLocation(config) {
  return withAndroidManifest(config, (cfg) => {
    const manifest = cfg.modResults;
    AndroidConfig.Manifest.ensureToolsAvailable(manifest);

    if (!Array.isArray(manifest.manifest['uses-permission'])) {
      manifest.manifest['uses-permission'] = [];
    }
    let scanPermission = manifest.manifest['uses-permission'].find(
      (item) => item.$?.['android:name'] === 'android.permission.BLUETOOTH_SCAN'
    );
    if (!scanPermission) {
      scanPermission = { $: { 'android:name': 'android.permission.BLUETOOTH_SCAN' } };
      manifest.manifest['uses-permission'].push(scanPermission);
    }
    scanPermission.$['android:usesPermissionFlags'] = 'neverForLocation';
    scanPermission.$['tools:targetApi'] = '31';

    if (!Array.isArray(manifest.manifest['uses-permission-sdk-23'])) {
      manifest.manifest['uses-permission-sdk-23'] = [];
    }
    for (const name of ['android.permission.ACCESS_COARSE_LOCATION', 'android.permission.ACCESS_FINE_LOCATION']) {
      let entry = manifest.manifest['uses-permission-sdk-23'].find(
        (item) => item.$?.['android:name'] === name
      );
      if (!entry) {
        entry = { $: { 'android:name': name } };
        manifest.manifest['uses-permission-sdk-23'].push(entry);
      }
      entry.$['android:maxSdkVersion'] = '30';
    }

    return cfg;
  });
};
