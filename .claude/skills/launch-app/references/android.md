# Android launch details

## After a FRESH install, the app sits at Android's runtime-permission dialog

A first launch on a newly-installed APK (fresh device, or after `pm uninstall` / "kill the app +
re-deploy") blocks on Android's **nearby-devices permission dialog** ("Allow RGB Glasses (Dev) to find,
connect to, and determine the relative position of nearby devices?"). Until it's granted the app can
scan nothing, so every BLE automation downstream fails in a way that does **not** mention permissions
and looks like a hardware/firmware problem — observed 2026-07-26: `scripts/re-pair.sh` reported
`WARN: board 'RGB Sunglasses Proto0 94E0' not listed / Connect not tappable within 25s` and gave up,
purely because the dialog was still up. A screenshot is the fast way to tell
(`mcp__execbro__android_screenshot`); the dialog belongs to `com.android.permissioncontroller`, not the
app, so app-level component queries won't surface it.

Grant it either way (API 31+ needs only the two nearby-devices permissions — the app never requests
location there, `.claude/rules/app-ble-connection.md`):

```bash
# Pre-grant before launching, so no dialog ever appears (preferred for unattended runs)
adb shell pm grant com.autom8ed.rgbsunglassesapp.dev android.permission.BLUETOOTH_SCAN
adb shell pm grant com.autom8ed.rgbsunglassesapp.dev android.permission.BLUETOOTH_CONNECT
```

or tap it: `tap(testID="com.android.permissioncontroller:id/permission_allow_button")`. **`pm grant` is
blocked on OxygenOS** — grant through Settings there
(`.claude/skills/drive-app/references/phones.md`). `pm uninstall` also **revokes** previously-granted
permissions, so a reinstall always re-arms this — budget for it whenever you redeploy, and don't read
the resulting "device not listed" as a BLE fault before ruling it out.

## `No development build (com.autom8ed.rgbsunglassesapp) for this project is installed`

The installed debug package is `com.autom8ed.rgbsunglassesapp.dev` (the dev-variant plugin adds an
`applicationIdSuffix`), and Expo CLI only reads the unsuffixed `applicationId` from `build.gradle`. The
fix is Expo's `--app-id` flag, which `app/scripts/launch-app.sh` already passes — so this error means
someone ran `expo run:android` directly. Root cause and history:
`.claude/rules/app-config-plugins.md` "Android dev variant".

## A device missing from `adb devices`

**Do NOT bring a device online yourself** — root `CLAUDE.md` "NEVER connect an ADB device yourself"
(`adb connect`/`pair`/`disconnect` are all off-limits; the old "always try `adb connect <ip:port>`
first" advice was withdrawn 2026-08-27). What you can do is diagnose and hand back:

- `adb devices` empty does **not** mean the device was never paired. Wireless-debugging pairing (the
  6-digit code flow) is remembered by the phone; only the TCP connection is container-local and drops
  on container restart. Missing local files like `~/.android/known_devices.xml` don't reliably reflect
  pairing state either.
- Wireless debugging rotates to a new random port whenever it restarts, and mDNS discovery does not
  work from inside the container, so the new port cannot be found locally.

Report that distinction — and the evidence (`adb devices -l`, ping, refused port) — then stop.
