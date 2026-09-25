# Manual pairing and passkey mechanics

Use `scripts/re-pair.sh` (the parent skill) first. This page is the background it
automates, and the fallback for when it can't drive the phone.

## How the passkey works

The firmware requires `BT_SECURITY_L4` (LE Secure Connections + bonding). On a fresh
pairing (board recently unpaired, or its bond info was cleared), the serial console prints
something like:

```
[00:23:51.161,041] <inf> bluetooth: Passkey for D0:49:7C:17:7B:E1 (public): 123456
[00:23:51.161,560] <inf> bluetooth: Peer needs to enter a pin code to pair
```

This is the firmware's own `passkey_display` auth callback (`fw/src/bluetooth.cpp`, IO
capability = Display-only, no `passkey_entry`/`passkey_confirm`/`pairing_confirm`
registered). The phone's Android BLE stack shows a native "Enter pairing code" system
dialog — not part of the companion app's own UI, so `mcp__execbro__android_screenshot`
won't necessarily surface it as an app screen; check for it explicitly — expecting that
exact 6-digit code typed in and submitted.

**Outside `re-pair.sh`, stop and ask the user before entering a passkey via ADB.** Ad-hoc
`adb shell input text` / `mcp__execbro__android_input_text` of a passkey you scraped by
hand is off-limits without the user's go-ahead — it is BLE pairing state on the one shared
physical phone, same spirit as the phone-reboot rule. `re-pair.sh` (user-commissioned
2026-07-11) *is* that go-ahead, standardized: an auditable script that self-gates on the
board + app locks.

## Manual fallback

For when `/re-pair` can't drive the phone (e.g. Settings-UI drift on the forget step).
First-time pairing accepts Android system prompts that are timing-sensitive:

1. After tapping CONNECT in the app, Android shows a **"Pairing request"** notification in
   the status bar shade.
2. Swipe down → tap the pairing notification, then enter the 6-digit code the board prints
   on serial (`Passkey for … : NNNNNN`) into the PIN dialog. Since the issue #232 firmware
   fix (`CONFIG_BT_SMP_SC_ONLY` + early L4 request) there is a **single, PIN-code prompt**
   — the old consent-only "Pair & connect" step no longer precedes it. If the firmware log
   shows `Pairing failed` without a disconnect, that's the firmware rejecting a raced-in
   unauthenticated attempt; Android retries with the PIN dialog on the same connection —
   keep going.
3. All of this must happen before Android times out waiting for user input and drops the
   connection (`BT_HCI_ERR_REMOTE_USER_TERM_CONN`, disconnect reason 19). A
   failed/cancelled PIN entry also gets a firmware-side disconnect (`BT_HCI_ERR_AUTH_FAIL`)
   instead of lingering at L1.

If a device has never been paired and `/re-pair` isn't being used, ask the user to watch
for and accept the Android pairing prompts themselves. Once paired, subsequent connections
complete automatically without any prompts. On iOS the pairing dialog leads discovery —
see `.claude/rules/app-ios.md`.

## When the automated forget fails ("bond still present")

A zombie system link or a pending app connect can block Unpair (see the parent skill's
"Unpair silently no-ops" gotcha). Try in this order:

1. The script's own ladder: force-stop the app, Unpair, `svc bluetooth disable` →
   `enable`, retry.
2. **`bt_adv off` on the board**, `--forget-only`, `bt_adv on`, then `--no-forget` (parent
   skill; 2026-08-15, the newer and lighter recipe).
3. Older recipe (2026-07-17, OnePlus 9 Pro), if `bt_adv` is unavailable: J-Link-halt the
   board (`halt` via JLinkExe — board lock required), `svc bluetooth disable` → `enable`,
   then IMMEDIATELY gear → Unpair (works only while the board is unreachable), then J-Link
   reset the board and `/re-pair --no-forget`.
