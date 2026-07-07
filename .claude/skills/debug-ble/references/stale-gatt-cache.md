# Stale Android GATT cache (issue #115) — mechanism details

Companion to SKILL.md §1, which keeps the signature and recovery decision rules; this
file carries the background needed only after matching that symptom.

## `bt_state` availability caveat (as of 2026-07 — re-verify)

`bt_state` ships in PR #117 (branch `pr2-ble-reliability`); on firmware built from
`main` before that merge the command doesn't exist. Fallback: `bt_conn_info`
(interval/latency/timeout only, no MTU) plus the app-side symptom — both `requestMTU`
and `discoverAllServicesAndCharacteristics` time out while the link stays up.

## Why stock Android auto-recovers

Service Changed indication + GATT DB hash (`CONFIG_BT_GATT_SERVICE_CHANGED` /
`CONFIG_BT_GATT_CACHING`). These are Zephyr defaults, not set in `fw/prj.conf` — verify
in `build/fw/zephyr/include/generated/zephyr/autoconf.h` after a build. OxygenOS-class
stacks (OnePlus 9 Pro) do NOT honor it; issue #115 has the two-phone evidence table,
including the negative result that no app-side connect option
(`refreshGatt`/`requestMTU` orderings) rescues a non-compliant stack.

## Transient-read errors are not stale cache

In `app/hooks/use-ble-connection.ts`'s discovery loop, individual `read()` failures are
caught and logged; `charInfo.value` stays null and renders as `false` via
`CharacteristicBoolean`. Isolated failures like that inside an otherwise successful
discovery are transient ATT failures — the stale-cache failure mode on non-compliant
stacks is a FULL discovery/MTU hang or timeout, never scattered per-characteristic
read failures.

## `refreshGatt: "OnConnected"`

Already passed by the app (grep `refreshGatt` in `app/hooks/use-ble-connection.ts`) —
keep it; it fixes the post-firmware-update `GATT_INVALID_HANDLE` case on compliant
stacks. Full entry: app/CLAUDE.md "Known Issues & Quirks".
