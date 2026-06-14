const { withAppBuildGradle, withDangerousMod } = require('@expo/config-plugins');
const fs = require('fs');
const path = require('path');

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

function withDebugResources(config) {
  return withDangerousMod(config, [
    'android',
    async (cfg) => {
      const androidRoot = cfg.modRequest.platformProjectRoot;
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
        const devXml = xml.replace(
          /<background[^>]*\/>/,
          '<background android:drawable="@color/iconBackgroundDev"/>'
        );
        fs.writeFileSync(path.join(debugMipmapDir, iconFile), devXml);
      }

      return cfg;
    },
  ]);
}

module.exports = function withDevVariant(config) {
  config = withDebugAppIdSuffix(config);
  config = withDebugResources(config);
  return config;
};
