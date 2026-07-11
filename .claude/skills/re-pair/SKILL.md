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

```bash
scripts/re-pair.sh
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

## Fragile point — the forget step

Forgetting a bond has no non-root ADB API, so the script drives Android Bluetooth
Settings via `uiautomator`. That UI is OEM-brittle; on failure the script **falls back to
asking you to tap Forget** and polls `dumpsys bluetooth_manager` until the bond actually
disappears (verify by state, not keypress). If the automated forget is flaky on this
phone, just run with `--manual-forget`.

## Success

The script drives `bt_state` over the UART and requires `CONNECTED` + `Security level: L4`
+ `ATT MTU > 23` (the `23` value is the split-brain signature and is a hard fail). On
success the app is connected and discovered (extension services render). It prints the
attempt count and the (ephemeral) passkey used, exit 0.
