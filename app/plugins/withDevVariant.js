const { withAppBuildGradle, withAndroidManifest, withDangerousMod } = require('@expo/config-plugins');
const fs = require('fs');
const path = require('path');

// Side-by-side install with the release APK: inject `applicationIdSuffix ".dev"`
// into the debug buildType so the dev app gets its own package id.
function withDebugAppIdSuffix(config) {
  return withAppBuildGradle(config, (cfg) => {
    const contents = cfg.modResults.contents;
    if (contents.includes('applicationIdSuffix ".dev"')) return cfg;

    cfg.modResults.contents = contents.replace(
      /(buildTypes\s*\{\s*debug\s*\{)/,
      '$1\n            applicationIdSuffix ".dev"'
    );
    return cfg;
  });
}

// Rewrite the app scheme in the source manifest to "rgbsunglassesapp.dev".
// Expo CLI reads this file to determine which scheme URL to use when launching
// the app, so the debug build gets its own scheme and the chooser dialog
// disappears when both the release and debug APKs are installed.
// The src/release/AndroidManifest.xml overlay (written by withDebugResources)
// restores "rgbsunglassesapp" for the release build at Gradle merge time.
//
// Must be "ensure exactly one", not "rewrite in place": `expo prebuild` (without
// --clean) applies mods on top of whatever is already on disk in android/, and
// @expo/config-plugins' own built-in `withScheme` base mod (android/Scheme.js,
// `setScheme`/`appendScheme`) only treats a scheme as already-present if it finds
// the literal "rgbsunglassesapp" from app.json's `expo.scheme` in the manifest. Since
// this function always rewrites that entry away to "rgbsunglassesapp.dev", the base
// mod sees "rgbsunglassesapp" missing on every subsequent prebuild and re-appends a
// fresh `<data android:scheme="rgbsunglassesapp">` entry via `appendScheme()` — which
// this function then rewrites to ".dev" too, adding one more duplicate per run
// (observed growing 1 -> 2 -> 3 across three consecutive prebuilds). Fix: remove
// every existing "rgbsunglassesapp"/"rgbsunglassesapp.dev" data entry from the
// intent-filter first, then insert exactly one ".dev" entry — self-healing even
// against duplicates left behind by prior (buggy) prebuild runs.
function withDevSchemeInManifest(config) {
  return withAndroidManifest(config, (cfg) => {
    const activities = cfg.modResults.manifest?.application?.[0]?.activity ?? [];
    for (const activity of activities) {
      for (const filter of activity['intent-filter'] ?? []) {
        if (!filter.data) continue;
        const isDevOrProdScheme = (data) =>
          data.$?.['android:scheme'] === 'rgbsunglassesapp' ||
          data.$?.['android:scheme'] === 'rgbsunglassesapp.dev';
        if (!filter.data.some(isDevOrProdScheme)) continue;
        filter.data = filter.data.filter((data) => !isDevOrProdScheme(data));
        filter.data.push({ $: { 'android:scheme': 'rgbsunglassesapp.dev' } });
      }
    }
    return cfg;
  });
}

function withDebugResources(config) {
  return withDangerousMod(config, [
    'android',
    async (cfg) => {
      const androidRoot = cfg.modRequest.platformProjectRoot;

      // --- debug res: label + dedicated DEV app icon ---
      const debugResDir = path.join(androidRoot, 'app/src/debug/res');
      const valuesDir = path.join(debugResDir, 'values');
      fs.mkdirSync(valuesDir, { recursive: true });

      fs.writeFileSync(
        path.join(valuesDir, 'strings.xml'),
        '<?xml version="1.0" encoding="utf-8"?>\n' +
          '<resources>\n' +
          '    <string name="app_name">RGB Glasses (Dev)</string>\n' +
          '</resources>\n'
      );

      // Debug build gets its own launcher icon (a dark-themed variant) so it's visually
      // distinct from the release install. Copy the DEV art in as a single nodpi drawable
      // (Android scales it per density — no per-bucket generation needed), then repoint the
      // adaptive-icon <foreground> at it in the debug mipmap overlay below. The debug res set
      // wins the resource merge, so the release icon is untouched. Only the adaptive (API 26+)
      // foreground is overridden; legacy pre-26 square/round icons keep the release art.
      const devIconSrc = path.join(cfg.modRequest.projectRoot, 'assets/images/appicon-dev.png');
      const debugDrawableDir = path.join(debugResDir, 'drawable-nodpi');
      fs.mkdirSync(debugDrawableDir, { recursive: true });
      fs.copyFileSync(devIconSrc, path.join(debugDrawableDir, 'ic_launcher_foreground_dev.png'));

      const mainMipmapDir = path.join(androidRoot, 'app/src/main/res/mipmap-anydpi-v26');
      const debugMipmapDir = path.join(debugResDir, 'mipmap-anydpi-v26');
      for (const iconFile of ['ic_launcher.xml', 'ic_launcher_round.xml']) {
        const mainIconPath = path.join(mainMipmapDir, iconFile);
        if (!fs.existsSync(mainIconPath)) continue;
        fs.mkdirSync(debugMipmapDir, { recursive: true });
        const xml = fs.readFileSync(mainIconPath, 'utf8');
        fs.writeFileSync(
          path.join(debugMipmapDir, iconFile),
          xml.replace(/<foreground[^>]*\/>/, '<foreground android:drawable="@drawable/ic_launcher_foreground_dev"/>')
        );
      }

      // --- release manifest overlay: swap scheme back to production value ---
      // The source manifest has "rgbsunglassesapp.dev" (written by withDevSchemeInManifest)
      // so the release APK needs to restore "rgbsunglassesapp" via the manifest merger.
      const releaseDir = path.join(androidRoot, 'app/src/release');
      fs.mkdirSync(releaseDir, { recursive: true });
      fs.writeFileSync(
        path.join(releaseDir, 'AndroidManifest.xml'),
        `<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
          xmlns:tools="http://schemas.android.com/tools">
  <application>
    <activity android:name=".MainActivity">
      <intent-filter tools:node="remove">
        <action android:name="android.intent.action.VIEW"/>
        <category android:name="android.intent.category.DEFAULT"/>
        <category android:name="android.intent.category.BROWSABLE"/>
        <data android:scheme="rgbsunglassesapp.dev"/>
      </intent-filter>
      <intent-filter>
        <action android:name="android.intent.action.VIEW"/>
        <category android:name="android.intent.category.DEFAULT"/>
        <category android:name="android.intent.category.BROWSABLE"/>
        <data android:scheme="rgbsunglassesapp"/>
      </intent-filter>
    </activity>
  </application>
</manifest>
`
      );

      return cfg;
    },
  ]);
}

const withDevVariantIos = require('./withDevVariantIos');

module.exports = function withDevVariant(config) {
  config = withDebugAppIdSuffix(config);
  config = withDevSchemeInManifest(config);
  config = withDebugResources(config);
  config = withDevVariantIos(config);
  return config;
};
