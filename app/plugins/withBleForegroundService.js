const { withAndroidManifest, withProjectBuildGradle } = require('@expo/config-plugins');

// notifee resolves its native core as `app.notifee:core:+` from a maven repo BUNDLED
// inside the npm package (android/libs). Its build.gradle tries to register that repo
// itself via `rootProject.allprojects { repositories { ... } }`, but in this Expo
// SDK 54 prebuild layout the app project's dependency resolution never sees it
// (observed: gradle searched only google/mavenCentral/jitpack/sonatype and failed
// with "Could not find any matches for app.notifee:core:+"). Register it explicitly
// in the root build.gradle's allprojects block instead.
function withNotifeeMavenRepo(config) {
  return withProjectBuildGradle(config, (cfg) => {
    const marker = '@notifee/react-native/android/libs';
    if (!cfg.modResults.contents.includes(marker)) {
      cfg.modResults.contents = cfg.modResults.contents.replace(
        /allprojects\s*\{\s*\n(\s*)repositories\s*\{/,
        (match, indent) =>
          `${match}\n${indent}  maven { url "\${rootDir}/../node_modules/${marker}" }`
      );
    }
    return cfg;
  });
}

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
  config = withNotifeeMavenRepo(config);
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
