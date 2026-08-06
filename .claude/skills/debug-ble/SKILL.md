---
name: debug-ble
description: "Diagnose device↔app Bluetooth problems: phone connects but app hangs or times out, GATT_INVALID_HANDLE, stale values after reconnect, toggle/Switch flicker, SMP request timeout, BLE writes rejected with androidErrorCode 252, slow discovery, and notify failures (Notify failed: -12 / bt_att: No ATT channel for MTU N) where the app silently never sees value changes. Covers both firmware and companion-app sides of a BLE symptom."
---

# Debug a device↔app BLE symptom

Every BLE symptom has a firmware side and an app side. Before touching either, read
`fw/CLAUDE.md` ("Serial Console (Zephyr Shell)") and `app/CLAUDE.md` ("Debugging BLE",
"Known Issues & Quirks", "Autonomous Agent Notes"). PR/issue numbers are background
(`gh issue view N` / `gh pr view N`), not facts to re-assert; when in doubt the code wins.

**Bash trap:** the PreToolUse guard `.claude/hooks/hw-lock-guard.sh` denies any Bash
command whose text matches `\bmcumgr\b` or `\badb\b` unless the board/app lock is held —
including an innocent `grep` of `app/services/mcumgr.ts`. Use the Read/Grep *tools* (not
Bash) on that file when you don't hold a lock.

## Symptom → diagnosis table

| Symptom | Diagnosis | Section |
| --- | --- | --- |
| Bonded phone connects, board LED shows connected, app hangs/times out on discovery or MTU | Android stale-GATT-cache split-brain (issue #115) | 1 |
| `GATT_INVALID_HANDLE` on reads/writes after a firmware update | Same stale-cache family (handles shifted) | 1 |
| UI shows stale/default values after reconnect | Client subscribed without an initial read (PR #78) | 2 |
| Switch/toggle snaps back mid-write, then corrects | Optimistic update applied after the `await` (PR #98, issue #91) | 3 |
| `SMP request timeout after 5000ms` when two SMP calls overlap | Requests not serialized through `requestChain` (PR #55) | 4 |
| EVERY SMP call times out (5000 ms) while the rest of the app works fine | Android's ~15-slot notification-registration cap exceeded; the SMP monitor is silently dropped | 4a |
| Write fails, `androidErrorCode: 252`, `attErrorCode: null` | Firmware rejected the write — this is deliberate, not a bug per se | 5 |
| Discovery/connect takes many seconds | Too many sequential ATT ops — do NOT parallelize, do NOT blame the JS bridge (issue #41) | 6 |
| Device vanished from scans after an app reload or a discovery throw | Orphaned native BLE link — force-stop the app | 7 |
| Firmware log: `bt_att: No ATT channel for MTU N` + `Notify failed ... -12`; app silently keeps a stale value | Notify payload exceeds negotiated ATT MTU (`bt_gatt_notify` can't fragment) | 8 |
| Panel keeps showing the pairing passkey (and shuffle is frozen) after a pairing that failed/timed out | Stale `BtPairing` indicator overlay — fixed in issue #242; on older firmware clear it with `anim indicator clear` | 9 |

## 1. Split-brain / stale Android GATT cache

Trigger: firmware GATT-layout change (add/remove/reorder) + already-bonded phone.

**Flipping a characteristic's `Notify` to false is a GATT-layout change.** This is the
non-obvious trigger and it has bitten this repo: `bt_service_cpp.h` emits the CCC
descriptor only under `if constexpr (Notify)`, so dropping notify deletes an attribute
and shifts every handle after it — exactly like removing a characteristic. PR #285
de-notified ~50 characteristics at once, which is a full-database shift for anyone
bonded to an earlier build. Expect the recovery below after that OTA on a
non-compliant stack, and check for it before concluding a post-update hang is a new bug.

Diagnose from the firmware shell (source of truth; board lock — see "Hardware-side
verification"): run `bt_state`. **`ATT MTU: 23` on a CONNECTED/encrypted (L4) link is
the signature** (healthy is `ATT MTU: 498`). If `bt_state` doesn't exist on the running
firmware, use the `bt_conn_info` fallback in `references/stale-gatt-cache.md` (PR #117).

Recovery is phone-stack-dependent (issue #115; `references/stale-gatt-cache.md`):
- **Stock Android (Pixel-class):** auto-recovers via Service Changed + DB hash. Nothing
  to do.
- **OxygenOS-class (OnePlus 9 Pro):** does NOT honor it. Only fix: forget device +
  re-pair — **automated via `/re-pair` (`scripts/re-pair.sh`)**, which forgets the bond
  and re-pairs hands-off (a local autoresponder types the board's passkey, winning
  Android's dialog timeout). No app-side connect option rescues it (verified in issue
  #115); don't burn time toggling `refreshGatt`/`requestMTU` orderings.

Scope check: stale-cache on non-compliant stacks is a FULL discovery/MTU hang or
timeout — isolated swallowed `read()` errors inside an otherwise successful discovery
loop (`app/hooks/use-ble-connection.ts`) are transient ATT failures, not issue #115
(details: `references/stale-gatt-cache.md`).

Do NOT reach for the app's `refreshGatt: "OnConnected"` — it was tried and **removed**
(`app/hooks/use-ble-connection.ts`, see the numbered rationale above `connectToDevice`).
Hardware testing showed it does not rescue the stale-cache hang on a non-compliant stack
while forcing a full re-discovery on every healthy connect, so it cost throughput and
bought nothing.

**Prevention:** the GATT table layout is a compatibility surface — append, never
insert/remove/reorder, in shipped firmware. See the `add-gatt-characteristic` skill
before changing any service.

## 2. Stale values after reconnect → read-then-subscribe

A reconnect changes no device state, so no notification fires; a subscribe-only client
shows stale defaults forever. **Rule: on (re)connect, read the current value first,
then subscribe.** In-tree precedent: `app/services/mcuboot-updater-client.ts` — grep
`statusChar.read()` (the read happens before `.monitor(...)`). Background: PR #78.
Residual window even then: `app/hooks/use-ble-connection.ts` reads values during the
multi-second discovery loop but attaches monitors only after the entire loop finishes —
a device-side change in that window (e.g. an extension fault flipping Is Active) is
silently missed until the next notification.

## 3. Toggle/Switch flicker on write

The optimistic value must be applied **synchronously before** `await
writeWithResponse(...)`, batched into the same render as `isUpdateInProgress: true`, and
reverted compare-and-swap-style inside a functional `setState` updater (so a device
notification landing mid-write is never clobbered by a stale revert). Canonical
implementation: `app/context/bluetooth-context.tsx` — grep `isUpdateInProgress`;
documented in app/CLAUDE.md "State Updates with Optimistic UI". Don't move the
optimistic update after the `await` — that reintroduces the flicker (PR #98 / issue #91).

## 4. SMP request timeout

`SMP request timeout after 5000ms` on overlapping calls means someone bypassed the
serialization. ALL SMP exchanges must queue through the `requestChain` promise chain in
`app/services/mcumgr.ts` (grep `requestChain` — read the field comment; use the
Read/Grep tools, not Bash, per the trap above). Never rely on its single
`responseResolver`/`responseRejecter` slots without that serialization — they're safe
only because the chain guarantees one in-flight exchange at a time; two overlapping
calls clobber the first's resolver (PR #55). Don't "fix" a client with a
per-sequence-number queue instead: the device itself only tracks one in-flight request.
A failed request must not poison the chain, and `destroy()` fail-fasts queued requests.

## 4a. EVERY SMP call times out, but the rest of the app is healthy

Distinct from §4 (which needs overlapping calls): here *no* SMP exchange ever completes —
image state, slot info, board detect, an OTA upload — while discovery, characteristic
reads/writes and every other notification work normally on the same connection.

**Cause: Android's Bluetooth stack has a fixed per-app GATT notification-registration
table (`BTA_GATTC_NOTIF_REG_MAX`, 15 entries in AOSP). Registrations past the cap fail
SILENTLY — the CCC descriptor write still returns success, so the firmware believes the
phone is subscribed and sends response notifications the phone's stack then drops.** The
SMP monitor is registered last (when the firmware-update modal opens), so it is always
the one that falls off the table. Root-caused 2026-08-05: the firmware had grown to 66
notifiable characteristics and the app auto-monitored all of them (67 CCC writes observed
on the wire).

Diagnose in this order — each step rules out a layer:

1. `mcumgr --conntype serial --connstring dev=/dev/ttyACM<N>,baud=115200 echo hi` +
   `image list`. Working = the SMP server, its handlers and the priority-3 workqueue are
   all alive, and the fault is BLE-transport-specific. (Board lock required.)
2. Count the app's registrations: `adb logcat | grep -c "setCharacteristicNotification.*enable: true"`
   and the app's own `Set up N characteristic monitors` line. **N + 2 (SMP + MCUboot
   Status, both modal-scoped) must stay ≤ 15.**
3. If you need proof the responses reach the phone: enable HCI snoop (Settings →
   Developer options → Bluetooth HCI snoop log → Enabled, then toggle Bluetooth),
   reproduce, `adb bugreport`, and parse
   `FS/data/misc/bluetooth/logs/btsnoop_hci.log` for ATT notifications on the SMP
   handle. Responses present in snoop but absent from `RxBle#Characteristic:
   Notification from ... da2e7828-…` in logcat *is* this bug — the stack received and
   discarded them.

**Prevention (the fix):** keep the firmware's notifiable count small. Only values that
change device-side and drive always-visible UI should notify; app-written tunables use
the app's read-back-after-write, and detail-screen telemetry re-reads on focus. Device
activation changes ride on Core Config's single **Active Animation** characteristic
(`fw/src/core_config.cpp` + `fw/src/animations/active_animation_binding.h`) rather than
one notify per animation — `fw/tests/bluetooth/core_config` pins that contract. Before
making anything else notifiable, re-count the budget.

## 5. Write rejected with androidErrorCode 252

`androidErrorCode: 252` (0xFC) with `attErrorCode: null` = the firmware returned
`BT_GATT_ERR(BT_ATT_ERR_WRITE_REQ_REJECTED)` (grep `WRITE_REQ_REJECTED` in
`fw/src/bluetooth/bt_service_cpp.h` and `fw/src/extensions/extension_bt.cpp`). Firmware
rejects deliberately when a write's hardware side effect fails — rolling back and
rejecting is the rule; "ACK then corrective notify" is banned (PR #106). App-side,
error mapping must probe `androidErrorCode` **and** the `reason` string, never just
`attErrorCode` — extend `describeWriteError` in `app/services/ble-errors.ts` (issue
#92, PR #100), not ad-hoc pattern-matching.

## 6. Slow discovery

Discovery is ~170+ sequential GATT reads and Android allows **one outstanding GATT op
per connection** — parallelizing is impossible, and the JS bridge is not the problem
(measured: ~89% of per-read latency is inside Android's native BLE stack — issue #41,
PRs #48/#54, background). The only real levers, already in place:
- Connection-interval tuning: `requestMTU: 247` + `requestConnectionPriority(High)` in
  `app/hooks/use-ble-connection.ts`; firmware side `bt_conn_le_param_update`. Android
  floors the interval at 15 ms — never expect 7.5 ms.
- **Reduce the NUMBER of ATT operations** — the per-service metadata blob (grep
  `parseMetadataBlob` in `app/services/ble-value-codec.ts`) replaced ~176 descriptor
  reads. Extend that approach for new bulk data; don't add per-characteristic reads to
  the discovery loop.

## 7. Orphaned connection after app reload / discovery throw

If the device stops advertising and a fresh scan finds nothing while the app thinks
it's disconnected, the OS still holds the native link. Fix: force-stop the app so the
OS drops the link, then relaunch. Both triggers (discovery-loop throw, `reload_app` /
mid-session reflash) and the exact force-stop procedure are in app/CLAUDE.md — see
"Known Issues & Quirks" and "BLE Link Can Get Orphaned by App Reloads, Not Just
Discovery Failures". Anything `adb`-shaped needs the `app` lock (see final section).

## 8. Notify silently dropped: payload exceeds negotiated MTU

`bt_gatt_notify()` cannot fragment — payload + 3-byte header must fit the CURRENT
negotiated ATT MTU. The `N` in the log line is the attempted PDU size (payload + 3),
**not** the MTU: `N` > 247 → payload itself oversized; 23 < `N` ≤ 247 → link likely
stuck at the 23-byte default (§1 stale-cache family); `N` = 23 exactly → NOT an
MTU-size problem but a teardown/bringup race (no usable ATT channel existed at the
instant the notify fired) — don't chase `requestMTU`. Verify on the serial shell (e.g.
`glim get_selected`), never from app UI alone. Full diagnostic: `references/notify-mtu.md`.

## 9. Panel stuck on the pairing passkey after a failed pairing

`onPairingCodeRequired()` raises the `BtPairing` panel overlay unconditionally (it has
to — the passkey renders on the panel). Before issue #242, `onConnected()` was the only
path that cleared it, so any pairing attempt that ended *without* reaching CONNECTED
(passkey never entered, `bt_smp: SMP Timeout`, peer walked away → `Disconnected (reason
19/22)`) left the overlay up indefinitely: the active animation stayed hidden and
shuffle stayed frozen (shuffle only steps while no indicator overlays the panel) until
the next successful connect or a manual `anim indicator clear`.

Diagnose over serial — the panel isn't visible to an agent, and `anim get` reports the
*underlying* animation, not the overlay, so the divergence is the tell:

```
bt_state            -> BT state machine: ADVERTISING / Active LE connection: none
anim get            -> rainbow          (what SHOULD be on the panel)
anim indicator get  -> bt_pairing       (what IS on the panel == the bug)
```

`anim indicator get` was added with the #242 fix; on firmware without it, compare the
physical panel against `anim get`. The fix clears a `BtPairing` overlay on the
CONNECTING → ADVERTISING transition, so on current firmware `anim indicator get` reads
`none` within milliseconds of the disconnect. Note this covers the *disconnect* path
only: a peer that fails L4 but never drops the link parks the state machine in
CONNECTING with the overlay legitimately still up.

## Hardware-side verification (source of truth)

Never conclude from app UI alone — cross-check against the firmware serial shell:
`anim get` (actual current animation), `bt_state` (link health, see §1 caveat),
`power bq status` (actual battery voltage/current vs. the app's battery card). Needs
the `board` lock (root CLAUDE.md "Hardware locking") and the `mcp__serial__*` tools per
fw/CLAUDE.md "Using the `mcp__serial__*` tools" — never raw Bash on `/dev/ttyACM*`.
Driving the phone (execbro/adb) needs the `app` lock; follow app/CLAUDE.md "Autonomous
Agent Notes" for tapping/coordinates — don't improvise.
