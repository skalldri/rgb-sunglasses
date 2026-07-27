const { withXcodeProject, withDangerousMod, IOSConfig } = require('@expo/config-plugins');
const { generateImageAsync } = require('@expo/image-utils');
const fs = require('fs');
const path = require('path');

const PROD_BUNDLE_ID = 'com.autom8ed.rgbsunglassesapp';
const DEV_BUNDLE_ID = `${PROD_BUNDLE_ID}.dev`;
const PROD_ICON_NAME = 'AppIcon';
const DEV_ICON_NAME = 'AppIcon-Dev';
const IMAGESET_DIR = 'Images.xcassets/AppIcon.appiconset';
const DEV_IMAGESET_DIR = 'Images.xcassets/AppIcon-Dev.appiconset';

function trimQuotes(value) {
  return typeof value === 'string' ? value.replace(/^"|"$/g, '') : value;
}

// Side-by-side install with a Release/TestFlight build: a distinct bundle id, home-screen
// label, and app icon for the Debug configuration. iOS has no per-buildType resource-overlay
// mechanism like Android's src/debug vs src/release, so this leans on per-configuration Xcode
// build settings instead (paired with `$(APP_DISPLAY_NAME)` in app.json's ios.infoPlist).
function withDebugBuildSettingsIos(config) {
  return withXcodeProject(config, (cfg) => {
    const project = cfg.modResults;
    const [, nativeTarget] = IOSConfig.Target.findFirstNativeTarget(project);
    const buildConfigs = IOSConfig.XcodeUtils.getBuildConfigurationsForListId(
      project,
      nativeTarget.buildConfigurationList
    );
    for (const [, xcConfig] of buildConfigs) {
      const isDebug = trimQuotes(xcConfig.name) === 'Debug';
      // Set both configurations explicitly (never rely on an unset custom build setting
      // resolving to something sensible - it resolves to "", which would blank the
      // Release app's home-screen label).
      xcConfig.buildSettings.APP_DISPLAY_NAME = isDebug
        ? '"RGB Sunglasses (Dev)"'
        : '"RGB Sunglasses"';
      xcConfig.buildSettings.ASSETCATALOG_COMPILER_APPICON_NAME = isDebug
        ? DEV_ICON_NAME
        : PROD_ICON_NAME;
      if (isDebug) {
        xcConfig.buildSettings.PRODUCT_BUNDLE_IDENTIFIER = `"${DEV_BUNDLE_ID}"`;
      }
    }
    return cfg;
  });
}

// Generates Images.xcassets/AppIcon-Dev.appiconset from assets/images/appicon-dev.png,
// mirroring whatever Contents.json Expo's own base icon plugin (withIosIcons) already wrote
// for AppIcon.appiconset. This dangerous mod is registered from app.json's plugins array,
// which @expo/prebuild-config's getPrebuildConfig.js resolves BEFORE it adds Expo's built-in
// iOS plugins (withIosExpoPlugins) - and @expo/config-plugins's withMod wraps each
// later-registered mod around the earlier one, so the later-registered (Expo base) mod
// actually executes first. That means AppIcon.appiconset/Contents.json is guaranteed to
// already exist on disk by the time this runs - verified against this project's installed
// @expo/prebuild-config, not assumed.
function withDevAppIconAssetIos(config) {
  return withDangerousMod(config, [
    'ios',
    async (cfg) => {
      const projectRoot = cfg.modRequest.projectRoot;
      const iosNamedProjectRoot = path.join(
        projectRoot,
        'ios',
        IOSConfig.XcodeUtils.getProjectName(projectRoot)
      );
      const baseContentsPath = path.join(iosNamedProjectRoot, IMAGESET_DIR, 'Contents.json');
      const baseContents = JSON.parse(fs.readFileSync(baseContentsPath, 'utf8'));
      const devIconSetDir = path.join(iosNamedProjectRoot, DEV_IMAGESET_DIR);
      fs.mkdirSync(devIconSetDir, { recursive: true });

      const devIconSrc = path.join(projectRoot, 'assets/images/appicon-dev.png');
      for (const image of baseContents.images) {
        const [width, height] = image.size.split('x').map(Number);
        const { source } = await generateImageAsync(
          { projectRoot, cacheType: 'icons-dev' },
          {
            src: devIconSrc,
            name: image.filename,
            width,
            height,
            resizeMode: 'cover',
            removeTransparency: true,
            backgroundColor: '#ffffff',
          }
        );
        fs.writeFileSync(path.join(devIconSetDir, image.filename), source);
      }

      fs.writeFileSync(
        path.join(devIconSetDir, 'Contents.json'),
        JSON.stringify(
          { images: baseContents.images, info: { version: 1, author: 'expo' } },
          null,
          2
        )
      );

      return cfg;
    },
  ]);
}

module.exports = function withDevVariantIos(config) {
  config = withDebugBuildSettingsIos(config);
  config = withDevAppIconAssetIos(config);
  return config;
};
