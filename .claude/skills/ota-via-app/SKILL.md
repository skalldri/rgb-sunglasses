---
name: ota-via-app
description: Run a firmware OTA update end to end through the companion app on the physical phone (the /submit-pr step 5a gate) — push the zip, drive the guided update flow, wait on the flow's own step titles, tap the manual restart, and verify the installed slot. HARDWARE skill — needs the board + app locks, a BLE-connected phone, and the built dfu_application.zip.
---

# Firmware OTA through the companion app

This is `/submit-pr` step 5a as an executable recipe. It exists because of a real
failure (2026-09-03): an agent started the upload, then armed a watcher on the board's
USB re-enumeration and on logcat words like `error`/`reset`. The first watcher fired on a
GitHub API response body; the second never fired at all. **The guided flow does not reset
the board on its own** — it stops at "Ready to restart" and waits for a tap — and the
`firmware-update/` screens log nothing. The maintainer had to report the upload was done.
The documented recipe for waiting (`/drive-app`, "Waiting for a screen change") already
existed; it was not consulted. Read `/drive-app` first, then follow this in order.

## Preconditions

- Both hardware locks held (root `CLAUDE.md` "Hardware locking"): `board` and `app`.
- App launched via `app/scripts/launch-app.sh` (never bare `expo run:android`), Metro
  reporting `packager-status:running`, execbro connected (`scan_metro`).
- Phone BLE-connected to the board: discovery finished (`Set up N characteristic
  monitors` in `adb logcat -s ReactNativeJS`, N ≤ 15) and `bt_state` on the serial shell
  shows `CONNECTED`, `L4`, an ATT MTU well above 23.
- The image to install: `fw/build/dfu_application.zip` from the build you are verifying.
  **Installing the image that is already running is a valid end-to-end check** (same
  bytes, same hash; MCUboot copies it and the slot hash is unchanged) — it exercises the
  whole SMP upload/stage/reset/verify path without changing what runs. Only use a
  different image when the test needs it; there is **no rollback** (overwrite-only
  bootloader — see `fw/docs/e2e-test-plan.md` E2E-02).
- **Close any `mcp__serial__*` connection before the restart step.** The board
  re-enumerates and the old connection goes stale (`fw/CLAUDE.md`, "USB re-enumeration").

## 1. Put the zip where the app's file picker can see it

```bash
adb push fw/build/dfu_application.zip /sdcard/Download/dfu_application_<tag>.zip
```

Use a distinctive name: the picker lists `Downloads` and there may be older zips there.

## 2. Open the flow and pick the file

1. `navigate(to: "/firmware-update")` — the landing page. Its version card populates over
   SMP (`Installed: vX.Y.Z · Board: Proto0`). If it shows `SMP request timeout` instead,
   stop: that is the notification-budget failure step 5a exists to catch (`/debug-ble` §4a).
2. `tap(testID: "fw-update-pick-zip")` ("Install from a .zip file"). The debug page's
   "Select Firmware Package (.zip)" reaches the same flow and also shows slot hashes first.
3. Android's DocumentsUI opens (a **native** screen — no fiber tree). `android_screenshot`,
   find the zip's row, then `tap(x, y, native: true)` with the execbro-space coordinates
   from that screenshot (the tool converts to device pixels; do not scale yourself).
   If the row is below the fold, `swipe` first and screenshot again.
4. The flow lands on **"Ready to install"** listing the images. Start it with the button's
   coordinates from `get_screen_state` (`tap(x, y)`), **not** `tap(text: "Install")`: that
   text also matches the title "Ready to install" and the caption "2 images will be
   installed" and the tool refuses as ambiguous (or, in the fiber strategy, presses the
   wrong node).

## 3. The state machine, and what to wait on

The flow title is the state — `STEP_TITLE` in `app/app/firmware-update/flow.tsx` is 1:1
with `FlowStep`. Poll the on-screen title, nothing else:

| title | what is happening | ends how |
| --- | --- | --- |
| Preparing update | parsing the zip | automatically |
| Ready to install | waiting for the Install tap | **your tap** |
| Uploading firmware | SMP upload, `Uploading image N of M…`, `NN%` | automatically |
| Preparing images | `image test` on the staged hashes | automatically |
| Ready to restart | staged; extension sync summary shown | **your tap** on `fw-update-restart` |
| Restarting device | `reset` sent; the board goes away | automatically |
| Waiting for device | reconnect loop | automatically |
| Verifying installation | slot hash / version check | automatically |
| Update complete / Update failed | terminal | — |

Measured 2026-09-03 (Pixel 9 Pro, 11.25 ms interval, 935 KB zip): upload ≈ 6 min,
restart → board back on USB 16 s, reconnect + verify < 1 min.

**Wait with the `/drive-app` step-title loop**, targeted per phase. Run it as a
background Bash task; it exits on the target, on `Update failed`, or on the deadline:

```bash
# Phase 1: upload + staging. Target = the manual gate.
TARGET="Ready to restart"; DEADLINE=$(( $(date +%s) + 900 ))
STEPS='Preparing update|Ready to install|Uploading firmware|Preparing images|Ready to restart|Restarting device|Waiting for device|Verifying installation|Update complete|Update failed'
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  if adb exec-out uiautomator dump /dev/tty 2>/dev/null > /tmp/w.xml; then
    title=$(grep -o 'text="[^"]*"' /tmp/w.xml | sed 's/text="//;s/"$//' | grep -xE "$STEPS" | head -1)
    pct=$(grep -o 'text="[0-9]*%"' /tmp/w.xml | head -1)
    echo "$(date +%T) [$title] $pct"
    [ "$title" = "$TARGET" ] && { echo REACHED; exit 0; }
    [ "$title" = "Update failed" ] && { echo TERMINAL-FAIL; exit 2; }
  fi
  sleep 10
done; echo TIMEOUT; exit 1
```

Then `tap(testID: "fw-update-restart")` and run the same loop with
`TARGET="Update complete"` and a 240 s deadline. Only in **this** phase is the board's USB
re-enumeration a meaningful signal (`lsusb | grep 2fe3:0001` disappears then returns,
~16 s); it is a useful cross-check, never the primary wait.

Things that do **not** work, each tried and failed:

- **Waiting for the board to reset after starting the upload.** It never does — the
  flow parks at "Ready to restart" until tapped.
- **`adb logcat` / `search_logs` for completion.** The flow screens emit no logs, and the
  MCUmgr client saturates the JS log buffer during the upload. Generic words (`error`,
  `reset`, `failed`) match GitHub API JSON the app fetched for the update check.
- **A single `uiautomator dump` sample.** Under BLE load it returns nothing parseable
  for 10–15 s at a time; an empty title means *unknown*, so keep polling. execbro's own
  accessibility strategy can report "Failed to get UI tree" at the same moments — the
  direct `adb` dump above still works.
- **`android_screenshot` in a loop.** Buries your context; use it once per phase at most.

## 4. Verify on the board, not just in the app

After "Update complete" (which already means the app verified the active slot's
`IMAGE_TLV_SHA256` against the zip — `app/CLAUDE.md`, "Firmware update"):

```bash
/check-hardware                                             # ports shift after the reset
python3 fw/tools/dump_dfu_tlv.py fw/build/dfu_application.zip | grep -A1 'SHA256'   # expected hash
mcumgr --conntype serial --connstring dev=<mcumgr port>,baud=115200,mtu=768 image list  # image=0 slot=0: active confirmed, same hash
adb logcat -d -s ReactNativeJS | grep 'Set up .* characteristic monitors' | tail -1     # post-reboot reconnect, still ≤ 15
```

Then the usual shell checks (`bt_state` CONNECTED/L4, `ext list` with no FAULTED,
whatever the change under test needs). On the OnePlus a post-OTA reconnect can wedge at
`ATT MTU: 23` (GATT-changing images especially) — `/re-pair`, and record it as the
known flake, not a regression. The Pixel re-discovers on its own.

## 5. Record it

The PR body's device-verification bullet names the phone, the zip, the phases reached,
the slot hash match, and the monitor count. If any phase was not reached, say which and
why — never "OTA verified" for an upload that stopped at "Ready to restart".

## 6. Release

Stop the `hold` tasks for both locks (that also stops Metro), and leave the board on
the image the PR is about. Restore anything you changed on the phone (e.g. a raised
`screen_off_timeout`).
