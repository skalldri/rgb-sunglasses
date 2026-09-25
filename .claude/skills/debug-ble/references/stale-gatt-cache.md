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

**No longer passed by the app** (issue #90): hardware testing showed it does not rescue
the stale-cache hang on a non-compliant stack, while forcing a full re-discovery on every
healthy connect. Compliant stacks recover through the firmware's Service Changed
indication instead; OxygenOS-class stacks need forget + `/re-pair`. Full entry: `.claude/rules/app-ble-connection.md` "Split-brain triggers".
