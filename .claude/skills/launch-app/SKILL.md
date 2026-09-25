---
name: launch-app
description: Launch or deploy the companion app on the physical phone (Android via app/scripts/launch-app.sh, iPhone via launch-app-ios.sh) — holds the app lock, runs the launcher as a harness background task, and knows when the launch actually finished. Use whenever the app must be running on a device for verification, /drive-app, /re-pair, /ota-via-app or /e2e-test.
---

# Launch the companion app on a phone

There is exactly one supported way to run the app on a device. Every shortcut below has already
broken a session. Platform detail: [references/android.md](references/android.md) (fresh-install
permission dialog, the `.dev` app id, ADB diagnosis) and [references/ios.md](references/ios.md)
(physical-iPhone deploys from the Mac).

## 1. Hold the `app` lock

There is only one physical phone shared across every agent worktree. `hold` is the only way to take
the lock (root `CLAUDE.md` "Hardware locking", `/hw-lock`):

```
Monitor(command: "scripts/hw-lock.sh hold app", persistent: true)
```
```bash
timeout 15 bash -c 'until scripts/hw-lock.sh check app >/dev/null 2>&1; do sleep 0.5; done'
```

`app/scripts/launch-app.sh` never acquires the lock; under Claude Code it verifies the lock is held
and hard-refuses otherwise, then records its own pid against the lock right before exec-ing into
Metro. So stopping the `hold` task (or a same-session `release app --force`) also stops Metro — but
Metro stopping or crashing on its own does NOT release the lock. A Metro/expo process left by an
earlier, now-dead session is killed automatically by the next `hold app`. The script has no guard
against a second Metro you started yourself in this session — don't start one.

## 2. In a worktree: `npm ci` first

A fresh worktree (`.claude/worktrees/<name>/app`) has **no `node_modules` and no `android/` of its
own**. Run `cd <worktree>/app && npm ci` — a **real** install (~30 s, reapplies the ble-plx patch via
`postinstall`). It is required for `jest`/`tsc` **and** for Metro. Eat this cost.

## 3. Launch as a harness-managed background task

```bash
app/scripts/launch-app.sh --device <device name>
```

Run it with Bash `run_in_background: true` and the bare command. It runs `expo prebuild`
(incremental, always — see `.claude/rules/app-config-plugins.md`), builds via gradle (~1 min once the
shared gradle cache is warm), starts Metro, installs the APK, and launches the app pointing at its own
Metro. Leave it running for the whole session — it owns Metro, but not the lock.

- **`--device` takes the model name** (`Pixel_9_Pro`, `LE2125`, …), never an ADB `ip:port`: Expo CLI
  matches against its own device list, so `--device 192.168.1.34:41181` fails with
  `CommandError: Could not find device with name: <ip:port>`. With exactly one device attached
  (`adb devices`), omit `--device` and Expo auto-selects it.

## 4. Know when it finished — it never exits

`launch-app.sh` ends by exec-ing into Metro, which stays in the foreground for the whole session, so
**never wait for the task (or an `until grep` loop) to finish** — that just blocks while the app has
been running all along (observed 2026-08-14: three stacked waiters on a launch that had already
succeeded). The launch is done when the **log content** says so: `BUILD SUCCESSFUL`, then
`Installing .../app-debug.apk`, then `Opening <pkg>/... on <device>`, then an `Android Bundled <n>ms`
line. Poll `http://localhost:8081/status` for `packager-status:running`, screenshot to confirm the app
loaded, and move on. Same reasoning for any foreground server task (Metro, a dev server, `hw-lock.sh
hold`).

Then check for the two things that sit on top of a fresh launch: Android's runtime-permission dialog
after a fresh install ([references/android.md](references/android.md)), and a restored navigation
stack (`.claude/skills/drive-app/references/navigation-traps.md`).

## DON'T — every one of these has caused a failure

- **NEVER call `npx expo run:android` directly** — it bypasses the lock check and the Metro-pid
  bookkeeping, so a second agent (or a forgotten earlier launch) can collide on the one phone. The
  hook denies it without the lock anyway.
- **NEVER symlink `node_modules`** from the main checkout into a worktree
  (`ln -s <main-checkout>/app/node_modules ...`). Gradle tolerates it, but
  Metro's resolver cannot resolve modules through a symlink whose realpath is outside the project root
  — you get `UnableToResolveError: Unable to resolve module ./app/node_modules/expo-router/entry` and a
  red-screen `development server returned response error code: 404`.
- **NEVER background `launch-app.sh` by hand with `&` and/or `> log 2>&1`.** The harness sees the
  wrapper "complete" immediately, loses track of it, and Metro gets reaped. The redirect also takes the
  app's `console.log` output (Metro's stdout) out of the tracked task stream; read app logs with
  `adb logcat -s ReactNativeJS` or execbro's `get_logs`/`search_logs`.
- **NEVER substitute `expo start --dev-client` + `adb reverse` + a
  `rgbsunglassesapp.dev://expo-development-client/?url=...` deep link** to avoid the native build — the
  dev client resumes its stale bundle without re-fetching, and you burn more time than a build costs.
  Don't pass `--android` to a separate `expo start` either (it launches Expo Go, not the dev client).
- **NEVER kill the underlying `expo run:android` process directly** to "restart Metro." Fix the real
  cause (usually a stale/symlinked `node_modules`), then stop the `launch-app.sh` task and relaunch it.
  Never `pkill` (root `CLAUDE.md` "Process management").

In short: **`npm ci`, hold `app`, then `app/scripts/launch-app.sh` as a background task.** No
symlinks, no manual daemonizing, no deep-link dance.
