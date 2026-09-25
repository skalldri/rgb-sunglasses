# Navigation traps and coordinate details

Incidents that look like app bugs and are not. The coordinate-space table itself is in the parent
skill ("Why the naive approaches fail").

## Coordinate details not in the parent table

- `inspect_at_point` takes **execbro px, not dp** — corrected 2026-08-07 against the live phone
  (`inspect_at_point(447, 1040)` returned the button under the finger; the dp-converted `(213, 495)`
  returned an unrelated `Card`). An earlier doc claimed dp and a `× 0.476` conversion; both were wrong.
  Verified three ways: `wm size`, a raw `screencap` PNG header, and execbro's own `convertedTo` echo.
- **Status bar**: 153 execbro px at the top (Pixel 9 Pro); app content starts below it.
  `measureInWindow` dp coordinates are relative to the content area (y=0 is below the status bar).
- On the OnePlus 9 Pro the pressables list that `android_screenshot` prints (e.g.
  `<AppButton/> "Connect" frame:(714,709 ...)`) reports coordinates *inflated* relative to the
  delivered image — passing them to `tap(x,y)` lands high/short. `tap(..., native=true)` and coordinate
  taps from the screenshot also misfired repeatedly there. Use `tap(text="...",
  strategy="accessibility")` first (it fires via the accessibility tree with no coordinate conversion
  and worked in every verified session), then `fiber-recipes.md`.

## `mcp__execbro__navigate` PUSHES a new instance (2026-08-29)

When the target route is not top-of-stack, `navigate` pushes a new instance — it does not pop back to
an existing one. Navigating calibrate → audio → (navigate "/device-state/audio-calibrate") stacked a
SECOND audio-calibrate instance; expo-router's dev-mode nav-state restore then resurrected the polluted
stack across a JS reload. With duplicate instances mounted, the app's ENTIRE touch pipeline wedged:
every Pressable showed press feedback (onPressIn) but onPress never fired — for real fingers and
injected taps alike — while JS stayed responsive. Looks exactly like a product bug and is not one (a
clean stack counted 80/80 real taps through the same code). Recover with `am force-stop` + relaunch,
which clears the restored stack. Prefer the app's own back-navigation / real UI taps over `navigate()`
for any screen already in the stack, and when a screen misbehaves, suspect the stack shape
(`get_screen_state` prints it) before the screen's code.

## A modal on top after a relaunch

After `adb shell am force-stop` + relaunch you may find a screen such as the **App Update** modal
(`app-update-modal`) on top of the Bluetooth tab — the stack reads like
`__root > (tabs) > app-update-modal > (tabs) > bluetooth` and blocks taps underneath. The launch-time
update check itself only renders a banner link (`app/hooks/use-app-update-check.ts`); a pushed modal
on relaunch comes from the same dev-mode nav-state restore as above. Dismiss it with
`adb shell input keyevent KEYCODE_BACK` (or `am force-stop` + relaunch to clear the restored stack)
before interacting with the Bluetooth or Controls screens. A BLE connection triggered before the modal
appeared is still live — the button shows "Disconnect" once the modal is cleared.

## Never chain blind injected input (2026-08-29)

An `input keyevent KEYCODE_APP_SWITCH` ×2 plus a pre-committed coordinate tap — with no screenshot
between steps — landed in the Settings app (open in recents) and the tap DISABLED Android Developer
options on the shared phone, nearly killing the ADB link. The re-read-before-every-tap rule applies
doubly to keyevents that change which app is foregrounded: screenshot between every injected step, or
hand the phone actions to the user when one is present.
