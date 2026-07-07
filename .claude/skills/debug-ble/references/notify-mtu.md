# Notify dropped: payload exceeds negotiated ATT MTU

Firmware-log signature: `bt_att: No ATT channel for MTU N` followed by the
characteristic-naming `Notify failed for "<Description>": -12 (payload P bytes)` printk
from `notify()` in `fw/src/bluetooth/bt_service_cpp.h`. The app sees nothing — no error
is surfaced; it just silently keeps the stale value.

## Why it happens

Unlike long reads/writes (which transparently fragment via blob-read /
prepare-execute-write), a single `bt_gatt_notify()` call cannot be split across ATT
PDUs: the whole payload plus the 3-byte notify header (1-byte opcode + 2-byte handle)
must fit in one packet bounded by the connection's CURRENT negotiated ATT MTU. When it
doesn't, Zephyr's `bt_att_create_pdu()` finds no channel and returns NULL, and
`bt_gatt_notify` returns `-ENOMEM` (-12). (Zephyr also logs
`No buffer available to send notification` for genuine buffer exhaustion — same -12,
different cause; the `No ATT channel` line is the MTU case.)

## Reading `N` diagnostically

`N` is the **attempted PDU size** (payload + 3), NOT the negotiated MTU (verified
against `bt_att_create_pdu()` in the NCS Zephyr host stack, as of 2026-07 / NCS
v3.1.1):

- **`N` > 247** — the payload itself is oversized. This was the real full-buffer
  DropdownList bug: `notify()` used to send `sizeof(storage_)` (e.g. 512 bytes for
  `GlimSelectionCharacteristic`) instead of the actual string length, so every notify
  failed even for a tiny selection list. Fixed in-tree (string-backed types now notify
  `strnlen`-based length — fw/CLAUDE.md `notify()` entry), but any new characteristic
  whose content can grow past ~244 bytes re-creates it.
- **23 < `N` ≤ 247** — the link is likely still at the 23-byte default MTU: the app's
  `requestMTU: 247` (grep `requestMTU` in `app/hooks/use-ble-connection.ts`) never took
  effect (SKILL.md §1 stale-cache family — cross-check with `bt_state`'s `ATT MTU`
  line), or the notify fired before the MTU exchange completed.

## Verification

Never conclude from the app UI alone — cross-check the characteristic's firmware-side
source of truth on the serial shell (e.g. `glim get_selected` for the GLIM selection
list; board lock required). Full background: fw/CLAUDE.md (`bt_service_cpp.h notify()`
bullet) and app/CLAUDE.md "Known Issues & Quirks" MTU entry.
