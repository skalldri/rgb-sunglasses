# Known test phones

**Any connected phone is viable** (root `CLAUDE.md` "Choosing which phone to use"). If a
phone answers over ADB, it is fair game — do not stop and ask merely because a particular
device is absent. The phones below are the *known* handsets, documented because their
quirks are known, not because they are the only permitted ones. Identify which phone is
attached (`adb devices -l` model field, or the `device` field in any execbro result) and
read everything else through that lens. Never connect, pair or re-dial a device yourself —
if nothing is connected, report what you saw and stop (root `CLAUDE.md`).

## Differences that matter

| | OnePlus 9 Pro (LE2125, OxygenOS / Android 14) | Pixel 9 Pro (stock Android 16) |
|---|---|---|
| BLE stack spec compliance | Non-compliant: ignores Service Changed; bonded reconnects **wedge** at `ATT MTU: 23` after a board reboot or GATT-changing reflash (split-brain). Only recovery: forget + `/re-pair` | Compliant: honors Service Changed, re-discovers on its own after a GATT-changing reflash — no forget/re-pair needed (verified by adding a characteristic and reflashing) |
| Coordinate taps from the screenshot image | **Unreliable** — land high/short; use `tap(text=…, strategy="accessibility")` or the fiber-walk recipes (`fiber-recipes.md`) | **Reliable** (verified 2026-07-31: repeated coordinate taps all landed); accessibility strategy and fiber-walk also work |
| Notification small-icon enforcement | Tolerant of a missing/invalid `smallIcon` | **Strict — a fatal app crash**, not a cosmetic issue: posting the BLE FGS notification without a resolvable `smallIcon` kills the process with `IllegalArgumentException: no valid small icon` (observed 2026-07-31 on every disconnect, because a stale `android/` lacked PR #224's `ic_stat_connection` drawable — `launch-app.sh` now always re-runs prebuild to prevent exactly this) |
| System-level bonded auto-connect | OxygenOS grabs an advertising bonded board at the system level (CONNECTED/L4, `ATT MTU: 23`, no app client) | Same phenomenon observed 2026-07-31 after a board power-cycle with a reconnect pending: serial showed CONNECTED/L4/MTU 498 while the app stayed "Reconnecting…" and the board never reappeared in scans (link held ⇒ board not advertising). Recovery: cancel the reconnect + `am force-stop` + relaunch + fresh Connect (or reset/power-cycle the board to free the link) |

Both phones: the fiber-walk `execute_in_app` recipes and `tap(text=…,
strategy="accessibility")` work — but accessibility/OCR matching only finds **on-screen**
elements, so scroll first for below-the-fold targets (e.g. the Battery card at the bottom
of Controls).

**Only a few phones have validated UI control paths.** On a phone with no validated recipe,
prefer the accessibility strategy first and say so in your report rather than trusting a
coordinate tap that silently missed.

## What a pass on each phone proves

A pass on the OnePlus carries over to the Pixel, while a pass on the Pixel does not by
itself prove the OnePlus path (it ignores Service Changed and wedges bonded reconnects).
When a change touches the GATT layout, say which phone you verified on and what that does
and does not cover. When it does not touch GATT, either phone is a genuine result.

## OnePlus 9 Pro (OxygenOS) — things that will otherwise cost an hour

- **`pm grant` is blocked** (`SecurityException: neither user 2000 nor current process has
  GRANT_RUNTIME_PERMISSIONS`), so the pre-grant recipe in
  `.claude/skills/launch-app/references/android.md` does not work there. Grant through the
  UI instead: `adb shell am start -a android.settings.APPLICATION_DETAILS_SETTINGS -d
  package:<pkg>` → Permissions → Nearby devices → Allow.
- **Every board reboot needs a re-pair**, not just a GATT-changing one — a plain
  reflash-and-reset wedges the bonded reconnect at `ATT MTU: 23`. Budget for `/re-pair`
  after each flash, and expect its automated forget to need the `bt_adv off` step (see
  `.claude/skills/re-pair/references/manual-pairing.md`); the forget only succeeds while
  the board is not reachable.
- **Auto-reconnect findings (hardware-verified 2026-07-17):** 12+ min backgrounded with the
  link CONNECTED/L4 and instant control on re-foreground; loop backoff/hedge sequencing
  correct; cancel stops the loop, aborts the pending connect, and stops the FGS. Also:
  - **OxygenOS auto-connects bonded boards at the SYSTEM level** — an advertising bonded
    board gets grabbed by the phone's stack itself (`bt_state`: CONNECTED/L4 but
    `ATT MTU: 23`, no app GATT client). Looks exactly like the split-brain; it isn't the
    app's doing (survives force-stop). A normal Connect tap re-adopts it via ble-plx's
    cancel-then-connect.
  - **Bonded reconnects after a board reboot or a phone BT-stack restart WEDGE** (MTU
    exchange never completes → discovery times out). The auto-reconnect loop correctly
    keeps retrying against it, but only forget + re-pair actually recovers — same
    pathology class as the issue-#90 stale-cache split-brain, now known to trigger WITHOUT
    any GATT layout change.
- **BT stack registration leak** (`SCAN_FAILED_APPLICATION_REGISTRATION_FAILED`, error 6):
  force-stop the app, then `adb shell am force-stop com.android.bluetooth` — `svc
  bluetooth disable`/`enable` alone did not clear it
  (`docs/agent-incidents.md#2026-08-12-bt-stack-registration-leak-on-the-oneplus`).
  Never `adb reboot` the phone (root `CLAUDE.md`).
