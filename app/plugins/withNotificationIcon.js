const { withDangerousMod } = require('@expo/config-plugins');
const fs = require('fs');
const path = require('path');

// notifee (@notifee/react-native, used by services/ble-foreground-service.ts for the
// BLE-connection foreground-service notification) has no Expo config plugin of its own,
// and defaults `android.smallIcon` to the literal resource name "ic_launcher" whenever a
// caller omits it - which this app's only displayNotification() call site did. Android's
// notification/status-bar icon renderer treats the small icon purely as an alpha mask
// (drawing a white silhouette from whatever is opaque), but "ic_launcher" resolves to the
// full-color, fully-opaque adaptive launcher icon - producing a solid white blob on
// spec-compliant renderers (verified: Pixel 9) instead of a readable icon.
//
// Fix: bake a dedicated, pre-rendered white-alpha notification icon (derived offline from
// assets/images/splash-icon-dark.png, which is already a transparent-background silhouette)
// into the standard Android density buckets, and reference it explicitly via
// `smallIcon: 'ic_stat_connection'` in ble-foreground-service.ts.
const DENSITIES = ['mdpi', 'hdpi', 'xhdpi', 'xxhdpi', 'xxxhdpi'];
const ICON_NAME = 'ic_stat_connection.png';

module.exports = function withNotificationIcon(config) {
  return withDangerousMod(config, [
    'android',
    async (cfg) => {
      const androidRoot = cfg.modRequest.platformProjectRoot;
      const assetsDir = path.join(cfg.modRequest.projectRoot, 'assets/images/notification');

      for (const density of DENSITIES) {
        const src = path.join(assetsDir, `ic_stat_connection-${density}.png`);
        const destDir = path.join(androidRoot, `app/src/main/res/drawable-${density}`);
        fs.mkdirSync(destDir, { recursive: true });
        fs.copyFileSync(src, path.join(destDir, ICON_NAME));
      }

      return cfg;
    },
  ]);
};
