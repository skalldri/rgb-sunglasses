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
function withDevSchemeInManifest(config) {
  return withAndroidManifest(config, (cfg) => {
    const activities = cfg.modResults.manifest?.application?.[0]?.activity ?? [];
    for (const activity of activities) {
      for (const filter of activity['intent-filter'] ?? []) {
        for (const data of filter.data ?? []) {
          if (data.$?.['android:scheme'] === 'rgbsunglassesapp') {
            data.$['android:scheme'] = 'rgbsunglassesapp.dev';
          }
        }
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

      // --- debug res: label, icon tint ---
      const debugResDir = path.join(androidRoot, 'app/src/debug/res');
      const valuesDir = path.join(debugResDir, 'values');
      fs.mkdirSync(valuesDir, { recursive: true });

      fs.writeFileSync(
        path.join(valuesDir, 'strings.xml'),
        '<?xml version="1.0" encoding="utf-8"?>\n' +
          '<resources>\n' +
          '    <string name="app_name">RGB Sunglasses (Dev)</string>\n' +
          '</resources>\n'
      );

      fs.writeFileSync(
        path.join(valuesDir, 'colors.xml'),
        '<?xml version="1.0" encoding="utf-8"?>\n' +
          '<resources>\n' +
          '    <color name="iconBackgroundDev">#FFB300</color>\n' +
          '</resources>\n'
      );

      const mainMipmapDir = path.join(androidRoot, 'app/src/main/res/mipmap-anydpi-v26');
      const debugMipmapDir = path.join(debugResDir, 'mipmap-anydpi-v26');
      for (const iconFile of ['ic_launcher.xml', 'ic_launcher_round.xml']) {
        const mainIconPath = path.join(mainMipmapDir, iconFile);
        if (!fs.existsSync(mainIconPath)) continue;
        fs.mkdirSync(debugMipmapDir, { recursive: true });
        const xml = fs.readFileSync(mainIconPath, 'utf8');
        fs.writeFileSync(
          path.join(debugMipmapDir, iconFile),
          xml.replace(/<background[^>]*\/>/, '<background android:drawable="@color/iconBackgroundDev"/>')
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

module.exports = function withDevVariant(config) {
  config = withDebugAppIdSuffix(config);
  config = withDevSchemeInManifest(config);
  config = withDebugResources(config);
  return config;
};
