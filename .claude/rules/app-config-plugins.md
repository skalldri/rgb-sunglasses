---
paths:
  - "app/plugins/**"
  - "app/app.json"
---

# Expo config plugins and the dev variant

Loads when you read a local config plugin or `app/app.json`. Launching (which runs `expo prebuild`
incrementally on every launch) is `/launch-app`.

## "Add if absent" config plugins cannot retrofit a stale `android/` — write local plugins as "ensure", not "add"

Because `app/scripts/launch-app.sh` runs `expo prebuild` without `--clean` on every launch, any
manifest/resource entry a plugin writes only when it's missing will, once written, never be corrected
by a later change to that plugin's config — the guard meant to make the plugin idempotent makes it
permanently stuck on whatever it wrote first. (A related stale-`android/` failure, the PR #224
notification-icon crash, is why `launch-app.sh` always re-runs prebuild at all.)

`react-native-ble-plx`'s own Expo plugin has this exact shape:
`addLocationPermissionToManifest`/`addScanPermissionToManifest`
(`node_modules/react-native-ble-plx/plugin/build/withBLEAndroidManifest.js`) only add the
`BLUETOOTH_SCAN`/`ACCESS_*_LOCATION` entries when absent, so flipping `neverForLocation` in `app.json`
after those entries exist from an earlier prebuild is silently a no-op —
`android:usesPermissionFlags="neverForLocation"` and `android:maxSdkVersion="30"` never land, and BLE
scanning on API 31+ ends up needing a location permission the app deliberately never requests (see
the comment in `requestAndroid31Permissions()`, `app/hooks/ble-manager.ts`). Fixed by
`app/plugins/withBleNeverForLocation.js`, registered **after** `"react-native-ble-plx"` in `app.json`'s
plugin list, which finds-or-creates the three entries and then unconditionally sets the required
attributes on them every prebuild — clean or incremental. When writing or reviewing any local plugin
under `app/plugins/`, prefer this "find-or-create, then set the required attributes unconditionally"
shape over "if not present, add" — the latter is only safe for plugins whose config never changes after
the first prebuild, which is not a safe assumption to make silently.

**A second instance, and a nastier shape: a mod can break a DIFFERENT mod's presence-check.**
`withDevSchemeInManifest` (`app/plugins/withDevVariant.js`) rewrote the
`<data android:scheme="rgbsunglassesapp"/>` intent-filter entry to `"rgbsunglassesapp.dev"`. That rename
defeated `@expo/config-plugins`' own built-in `withScheme` base mod (`android/Scheme.js`), whose
`setScheme`/`appendScheme` only skip re-adding a scheme they find already present. Having renamed the
configured scheme away, our mod made it look *missing* on every subsequent prebuild, so the base mod
re-appended a fresh one — which our mod then renamed too, adding one duplicate `<data>` entry per
prebuild (measured 1 → 2 → 3 across three runs on an unclean `android/`). Fixed by removing every
existing `"rgbsunglassesapp"`/`".dev"` entry and inserting exactly one, which self-heals manifests
already carrying duplicates and leaves no "missing scheme" state for the base mod to react to.

What generalises: it is not enough for your own mod to be idempotent in isolation. If it mutates
something another mod uses as ITS presence-check, you have made that mod non-idempotent instead, and
the damage shows up in a file neither author is looking at. In the same file, `withDebugAppIdSuffix`
was already correct (`if (contents.includes('applicationIdSuffix ".dev"')) return cfg;`) and
`withDebugResources` overwrites rather than appends, so both are idempotent by construction. Duplicate
`<data>` elements are harmless at runtime — Android tolerates them — so this class is prebuild
determinism and hygiene rather than a crash, but it is worth checking for in any new manifest or
gradle mod, because the failure is invisible until someone diffs a generated file.

## Android dev variant: the `.dev` application id

`app/plugins/withDevVariant.js` injects `applicationIdSuffix ".dev"` into the debug build type
(`android/app/build.gradle`) so the debug and release APKs install side-by-side with distinct
icons/schemes — the installed debug package is `com.autom8ed.rgbsunglassesapp.dev`. Expo CLI's
package-id resolver (`@expo/config-plugins`' `Package.getApplicationIdAsync()`, called from
`AndroidAppIdResolver`) only regexes the literal `applicationId '...'` line out of `build.gradle`, with
no knowledge of `applicationIdSuffix`. So a bare `expo run:android` always computes the unsuffixed id,
finds it isn't installed (`PlatformManager.openProjectInCustomRuntimeAsync` →
`isAppInstalledAndIfSoReturnContainerPathForIOSAsync`), and throws `CommandError:
No development build (com.autom8ed.rgbsunglassesapp) for this project is installed` — even right after
its own successful install. The real fix is Expo's `--app-id <appId>` flag, which
`app/scripts/launch-app.sh` already passes (`--app-id com.autom8ed.rgbsunglassesapp.dev`). The manual
`adb install` + `monkey`/`android_launch_app` path still works as a fallback (e.g. if Metro won't
start).

## iOS dev variant (`app/plugins/withDevVariantIos.js`)

Composed into `withDevVariant` alongside the Android-only steps: a Debug-configuration build gets
bundle id `com.autom8ed.rgbsunglassesapp.dev`, home-screen label "RGB Glasses (Dev)", and the same dark
`appicon-dev.png` art Android uses (regenerated into a second `AppIcon-Dev.appiconset` from the
existing `AppIcon.appiconset`'s `Contents.json`), so `expo run:ios`'s Debug build and a
TestFlight/Release build can coexist on one device. Unlike Android, this is done entirely via
per-configuration Xcode build settings (`PRODUCT_BUNDLE_IDENTIFIER`, `APP_DISPLAY_NAME` referenced from
`app.json`'s `ios.infoPlist.CFBundleDisplayName` as `$(APP_DISPLAY_NAME)`,
`ASSETCATALOG_COMPILER_APPICON_NAME`) rather than a Gradle-style per-buildType resource overlay —
iOS's Debug/Release are configurations of one target sharing one Info.plist, so there's no `src/debug`
equivalent. Only `Debug` gets the `.dev`-suffixed id/name/icon; `Release` (the TestFlight archive,
`-configuration Release` only) is untouched.

Deliberately **not mirrored**: Android's `rgbsunglassesapp` → `rgbsunglassesapp.dev` URL-scheme
rewrite. That trick exists solely to avoid a chooser-dialog collision in `expo run:android`'s
deep-link launch path and to disambiguate the Android-only self-update deep link
(`app/services/app-update.ts`). `expo run:ios --device` doesn't launch via a scheme-based deep link —
it reads the `CFBundleIdentifier` out of the freshly-built `.app`'s Info.plist and installs/launches
directly — so there's no iOS consumer to disambiguate for.
