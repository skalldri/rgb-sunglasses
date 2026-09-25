# Physical-iPhone dev builds (verified 2026-07-11, iPhone 15 / iOS 26.5, Xcode 26.2)

iOS builds only on a Mac (the Mac Mini). One-time setup and CI/TestFlight: `.claude/rules/app-release-ci.md`;
iOS BLE behaviour: `.claude/rules/app-ios.md`. The BLE path needs a **physical iPhone** — the
Simulator has no radio.

**Deploying a local dev build to a physical iPhone — use `app/scripts/launch-app-ios.sh --device <UDID>`,
never a bare `npx expo run:ios`.** The wrapper is the iOS sibling of `launch-app.sh`: it verifies the
`app` hardware lock (agent sessions), always re-runs `expo prebuild --platform ios` (incremental) so
config-plugin output lands no matter how old the checked-out `ios/` is, and then **asserts the Debug
configuration carries the `.dev` bundle id before building** — refusing loudly otherwise. That assert
exists because of a real incident (2026-08-13): a bare `expo run:ios` against a stale `ios/` predating
`withDevVariantIos` built Debug under the PRODUCTION bundle id and silently replaced the TestFlight
install on the shared iPhone. (Verified: the incremental prebuild fully retrofits even a tree that
predates the #320 rename — such a tree keeps its old `RGBSunglasses.*` project name, which is why the
wrapper globs `ios/*.xcodeproj` rather than hardcoding a name.) Run it as a harness-managed background
task, same as the Android wrapper.

- Pass the **traditional hardware UDID** from `xcrun xctrace list devices` (`00008120-…`), NOT the
  CoreDevice UUID that `xcrun devicectl list devices` prints — Expo CLI doesn't match the latter. (Expo
  also warns `Unexpected devicectl JSON version` on Xcode 26; device matching by UDID still works.)
- Expo refuses to build with "No code signing certificates are available" and will NOT mint one.
  First-time bootstrap: sign into Xcode → Settings → Accounts, make sure `ios.appleTeamId` is in
  `app.json` (it is; prebuild bakes it into the project as `DEVELOPMENT_TEAM` — without it xcodebuild
  fails with "requires a development team"), then run
  `xcodebuild -workspace ios/RGBGlasses.xcworkspace -scheme RGBGlasses -configuration Debug
  -destination 'id=<UDID>' -allowProvisioningUpdates build` once — that creates the Apple Development
  cert + device-registered profile, after which `expo run:ios --device` works normally.
- Expo's auto-launch after install can silently no-op on physical devices; launch explicitly with
  `xcrun devicectl device process launch --device <UDID> com.autom8ed.rgbsunglassesapp.dev` — note the
  **`.dev`** suffix: the wrapper builds Debug, which installs under the dev-variant bundle id. Launching
  the bare production id either errors ("no such app") or launches a leftover TestFlight install — and
  looks exactly like a failed deploy.
- **Local Network permission**: the app can't reach Metro (LAN IP in the app's `ip.txt`) until the user
  accepts iOS's local-network prompt on first launch — the symptom is "app launches, Metro never
  receives a bundle request". Phone and Mac must share the Wi-Fi network.
- execbro works against a physical iPhone at the **JS level only** (Metro CDP: `scan_metro`,
  `get_logs`, `execute_in_app`); screenshots/tap drivers are simulator-only.
- The board's serial shell and the rest of the firmware loop on the Mac:
  `.claude/skills/flash-and-verify/references/macos-host.md`. `scripts/hw-lock.sh` works on macOS and
  the same lock discipline applies.
