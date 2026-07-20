---
name: re-pair
description: "Fast, hands-off BLE re-pairing of the shared Android phone to the dev board — forgets the stale bond and re-pairs, with a local autoresponder typing the board's random passkey so it beats Android's pairing-dialog timeout. Use for the OnePlus/OxygenOS stale-GATT split-brain (bt_state CONNECTED/L4 but ATT MTU 23), after a fresh flash that erased bonds, or any 'Pairing failed'/reason-19 loop."
allowed-tools: Bash, mcp__serial__serial_close
---

# Fast automated BLE re-pairing

`scripts/re-pair.sh` runs the whole **forget → re-pair → verify** loop hands-off. The
board displays a fresh **random** 6-digit passkey each pairing; the script arms a serial
watcher on the shell UART *before* triggering the connect, then a local autoresponder
types the passkey into Android's dialog the instant it appears — winning the pairing
timeout race that a read-and-relay human/agent loop loses (the app cancels after ~15s →
board logs `Disconnected (reason 19)`).

No firmware change — random passkey and full L4/authenticated security are unchanged.

## When to use

- **OnePlus/OxygenOS stale-GATT split-brain** (`bt_state` shows `CONNECTED`/`L4` but
  `ATT MTU: 23` after a GATT-changing reflash) — see `/debug-ble`. The only fix is
  forget + re-pair; this automates it.
- After a flash that **erased the settings/bond partition**, or any repeated
  `Pairing failed` / reason-19 pairing loop.

## Preconditions (in order)

1. **Hold BOTH the `board` and `app` locks** (it touches serial and adb):
   ```
   Monitor(command: "scripts/hw-lock.sh hold board app", persistent: true)
   ```
   ```bash
   timeout 15 bash -c 'until scripts/hw-lock.sh check board >/dev/null 2>&1 && scripts/hw-lock.sh check app >/dev/null 2>&1; do sleep 0.5; done'
   ```
   The script hard-refuses if either lock is missing.
2. **Close any open MCP serial connection** to the shell port first
   (`mcp__serial__serial_close`) — the script opens and OWNS the UART, and two readers
   race. Preflight aborts (via `fuser`) if the port is still busy.
3. **App installed + Metro running** (`app/scripts/launch-app.sh`). The script force-stops
   and relaunches the app to trigger a clean connect; it warns (non-fatal) if Metro isn't up.

## Run

**ALWAYS pass `--device-name "<exact advertised name>"`.** Never rely on the script's
default — a wrong name makes it forget some *other* board's bond and then wait for a
board that never advertises (every attempt times out). Get the exact name first from the
board's own shell (`bt_state` / the boot `BT device name:` log) or the phone's bond list
(`adb shell dumpsys bluetooth_manager | grep "RGB Sunglasses"`), e.g. `RGB Sunglasses
Proto0 94E0`. The suffix is per-board (derived from its BT identity), so it differs
between physical boards.

```bash
scripts/re-pair.sh --device-name "RGB Sunglasses Proto0 94E0"
```

Flags:

| flag | effect |
| --- | --- |
| `--no-forget` | skip the phone-side forget (bond already gone / not needed) |
| `--forget-only` | forget the bond and stop (no re-pair) |
| `--manual-forget` | skip UI automation of forget; prompt the human, poll until the bond disappears |
| `--attempts N` | pairing attempts before giving up (default 3) |
| `--keyevents` | type the passkey as per-digit key events instead of `input text` |
| `--timeout-connect S` | per-attempt dialog deadline (default 15s, matches the app) |
| `--serial <s>` / `--device-name "<n>"` | adb target / advertised name overrides |

## Implementation

`scripts/re-pair.sh` is a thin wrapper around **`scripts/re-pair.py`** (moved to Python so
the safe, device-specific forget could be written and tested properly). Verified end-to-end
on both a OnePlus 9 Pro (OxygenOS — persistent on-screen pairing dialog) and a Pixel 9 Pro
(stock Android — the pairing prompt is a transient heads-up notification the responder pulls
from the notification shade). The passkey autoresponder handles both dialog forms.

## The forget step is SAFE by construction

Forgetting a bond has no non-root ADB API, so the script drives Android Bluetooth Settings
via `uiautomator`. Because a phone can have dozens of bonds (and several boards share the
"RGB Sunglasses Proto0" prefix), the forget is strictly device-specific:

- Matches the target by its **EXACT full name** (`--device-name`, default
  `RGB Sunglasses Proto0 8996`) — never a prefix/substring.
- Expands the full device list ("See all" on stock Android) and scrolls to find **that
  device's own gear** (identified by the device name), never "the first gear".
- **Taps Forget ONLY after verifying the opened details page's header shows that exact
  name** — otherwise it backs out and falls back to a manual prompt. This gate is what
  guarantees it never forgets the wrong device (a car, earbuds, another board).

On any failure it **falls back to asking you to tap Forget** and polls `dumpsys
bluetooth_manager` until the bond actually disappears (verify by state, not keypress).
`--manual-forget` skips straight to that; `--no-forget` skips it entirely.

### Gotcha: Unpair silently no-ops while a client is auto-connecting

Observed 2026-07-17 (OxygenOS / OnePlus 9 Pro): with the companion app running, its
BleManager keeps a **pending LE connection** to the board permanently armed (`dumpsys
bluetooth_manager` shows the board under `devices attempting connection`). In that
state, Settings' **Unpair tap silently does nothing** — the details page is correct,
the tap lands, the UI navigates back, and the bond is still there. This is why the
automated forget used to "succeed" into the manual fallback.

The script now handles it itself: it **force-stops the app before the forget** (the
relaunch happens in the pairing phase anyway), and if the unpair still doesn't stick it
**cycles the Bluetooth stack (`svc bluetooth disable`/`enable`) and retries once**
before falling back to manual. If you're forgetting the bond by hand instead: kill the
app first, and if the bond survives an Unpair, toggle Bluetooth off/on and try again —
don't keep tapping Unpair, it will never take while the pending connect exists.

## Success

The script drives `bt_state` over the UART and requires `CONNECTED` + `Security level: L4`
+ `ATT MTU > 23` (the `23` value is the split-brain signature and is a hard fail). On
success the app is connected and discovered (extension services render). It prints the
attempt count and the (ephemeral) passkey used, exit 0.
