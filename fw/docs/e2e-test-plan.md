# AI-agent-driven end-to-end test plan (app + device)

plan-version: 1.0.0

The second deliverable of issue #333: combined app+device flows that the
shell-only HIL suite (`fw/tests_device/`, see `fw/docs/on-device-testing.md`)
structurally cannot reach — everything that needs the companion app on the
physical phone talking BLE to the board. Executed by an AI agent driving the
phone (execbro/ADB per `/drive-app`) and the board's serial shell
simultaneously; a human can follow the same steps.

## Execution contract

**Locks**: hold BOTH `board` and `app` for the whole run
(`Monitor(command: "scripts/hw-lock.sh hold board app", persistent: true)`).

**Conventions**: `/drive-app` for all UI steps — `tap(testID=...)` first,
`tap(text=..., strategy="accessibility")` second, NEVER `tap(component=...)`
or index taps. Poll `get_screen_state(pressablesOnly: true)`; no screenshot
loops; no log polling during SMP uploads (the JS log buffer saturates).
Pairing goes through `/re-pair` — never hand-typed passkeys.

**Pass criteria are programmatic**: every scenario's verdict comes from
serial assertions (via `mcp__serial__*`) and app-state reads, not from
eyeballing screenshots. Screenshots are evidence, not criteria.

**Results**: one JSON object per run, posted as a comment on the dedicated
tracking issue (create on first run: "E2E run results — issue #333"):

```json
{
  "run": {"date": "...", "fw_version": "...", "app_commit": "...",
           "phone": "...", "plan_version": "1.0.0"},
  "scenarios": [
    {"id": "E2E-01", "status": "pass|fail|skip|flake|deferred",
     "evidence": ["serial excerpts", "screenshot paths"], "notes": "..."}
  ]
}
```

`flake` = failed, then passed on retry with a known-flake signature (each
scenario lists its own). Anything else that fails twice is `fail`. `skip` = a
precondition made it inapplicable (e.g. no battery); `deferred` = deliberately
not attempted this pass (e.g. a board/bond-destructive scenario held for a
supervised run, or E2E-03 which is human-supervised by design).

**Version preconditions**: record `serial print`, `mcuboot_version`, and
`kernel version` from the shell plus the app build (Settings screen or
`app/package.json`) into `run` before starting.

---

## E2E-01 — Pairing from a cold bond

Covers: #244/#232 (single L4 passkey prompt), the `/re-pair` flow itself.

1. Precondition: board advertising (`bt_state` → ADVERTISING). If bonded,
   that's the point — this scenario intentionally clears it.
2. Run `/re-pair` (forgets the stale bond, re-pairs with the autoresponder).
3. Serial: `bt_state` → `CONNECTED`, security `L4`, ATT MTU ≥ 247
   (498 = healthy; 23 = split-brain, see flakes).
4. App: device page renders characteristic values (no infinite spinners).

Pass: step 3 values + step 4 within 60 s of re-pair completing.
Known flake: OnePlus/OxygenOS stale-GATT split-brain (`ATT MTU: 23` on an
otherwise CONNECTED/L4 link) after a GATT-layout-changing reflash → rerun
`/re-pair` once; if it recurs, `fail` with the `bt_state` dump.
Cleanup: none (leaves a fresh healthy bond).

## E2E-02 — Full OTA firmware update via the app wizard

Covers: #288/#287 (10-state update wizard), extension re-sync
(`app/services/extension-sync.ts`), post-OTA reconnect.

⚠️ Proto0 is MCUboot OVERWRITE_ONLY: **there is no rollback**. Use an image
built from the same source at a tweak-bumped version. Recovery from a bad
image = `fw/scripts/mcumgr-flash.sh --recovery` or J-Link reflash.

1. Precondition: E2E-01 healthy; a `dfu_application.zip` at a version ≠
   running version (build with a bumped `VERSION` tweak).
2. App: navigate to the firmware-update flow, select the image, start.
   The step title is 1:1 with the `FlowStep` enum — poll the title text.
3. Expect the upload phase (minutes; do NOT poll logs), then the app-driven
   `image test` + reset: the BLE drop is the signal (20 s window), then
   reconnect (patience 180 s).
4. Serial after reboot: `mcuboot_version` / `kernel version` show the new
   version; `mcumgr image list` (host-side, SMP port) shows image 0 confirmed.
5. App: wizard reaches its done state; extension sync pass reports all
   hashes matched (or re-uploaded then matched).
6. Serial: `bt_state` CONNECTED/L4 again; `ext list` no FAULTED.

Pass: 4 + 5 + 6 all true. Known flake: post-OTA stale-GATT on the OnePlus
(handle shifts, #115/#43) → forget + `/re-pair`, note as `flake`, continue.
Cleanup: none (device is on the new version — record it in the results).

## E2E-03 — Bootloader update (HUMAN-SUPERVISED)

Covers: #73 two-stage MCUboot updater (GATT service + `/NAND:/mcuboot.bin`
staging + `mcuboot_update` shell surface).

**Never run unattended.** A power loss between `commit` starting and
finishing bricks the board to J-Link-recovery-only. The agent may execute
steps ONLY with the user's explicit go-ahead at launch and with the J-Link
attached; the user stays present. Recovery: `fw/docs/flashing-without-jlink.md`
+ J-Link reflash per `/flash-and-verify`.

1. Stage: app uploads `mcuboot-<v>-proto0.bin`; or host stages to
   `/NAND:/mcuboot.bin` over USB MSC (then reboot to re-mount FAT).
2. Serial: `mcuboot_update verify` → GRMB header fields + CRC OK.
3. App: updater flow → requestUpdaterReboot → device reboots (flash
   unlocked) → reconnect → unlock → begin → chunks → validate → TWO confirm
   dialogs → commit.
4. Serial after commit + reboot: `mcuboot_version` shows the new bootloader
   version; app image still boots; `bt_state` healthy after re-pair if needed.

Pass: step 4. Cleanup: none.

## E2E-04 — Extension install / update / remove over BLE

Covers: #303/#305 (FILE_MGMT group 64 LIST/DELETE semantics, retire-first
delete, the RETIRED-ghost rule), extension-sync hash comparison.

1. Precondition: E2E-01 healthy; board provisioned with baseline extensions.
2. Install: app uploads a NEW `.llext` (e.g. cpptest if absent, or a rebuilt
   hello under a new name). App list shows "takes effect after restart";
   serial `ext list` does NOT yet show it.
3. Reboot (app restart-device path or `kernel reboot warm`). App list shows
   it loaded; serial `ext list` shows the new slot.
4. Update: re-upload the same name with a different build; app shows
   pending-restart for it (hash mismatch drives re-upload); reboot; loaded.
5. Remove: app deletes it. Serial: slot RETIRED (activation rejected) until
   restart; after reboot it is gone from `ext list` AND from `/NAND:/ext`
   (`fs ls /NAND:/ext`).
6. Ghost rule: remove-then-reinstall same name before reboot reads as a
   fresh "takes effect after restart" file, NOT as "removed".

Pass: every state transition matches on BOTH sides (app list rendering +
serial). Cleanup: restore the baseline extension set (reprovision if needed).

## E2E-05 — Animation controls matrix (all CPF input types)

Covers: "all accessible animation controls" (#333), #149 text commit
platform differences, #237 optimistic updates, #98 Is Active flicker.

For EACH input type: app write → serial cross-check → app re-read matches.

| Type | Example control | Serial cross-check |
| --- | --- | --- |
| bool (Switch) | any animation's Is Active | `anim get` |
| uint32 (slider/stepper) | Core Config brightness | `settings read` value or re-read via app |
| float32 | animation param (e.g. tilt gain) | `ext param` / adapter readback |
| utf8 text | Text animation string | re-read + panel renders (evidence photo optional) |
| color | any color param | `ext param` shows 0xRRGGBB |
| dropdown | GLIM selection | `glim get_selected` |
| slot playlist | Text/My Eyes queue | Up Next / Now Playing reads |

Pass: every row round-trips with no value fights (#122 — the value must not
revert while typing continues) and no Switch flicker. Cleanup: restore
defaults for every touched control (brightness back to original, etc.).

## E2E-06 — Is Active mirror + fault recovery (#89)

The regression only a BLE READ can see: a dead extension whose Is Active
characteristic still reads true (the ignored `-ENOENT` bug).

1. App: toggle hello ON → serial `ext list` shows `[active]`; app toggle ON.
2. App: set hello's Crash param → sandbox faults. Serial: `FAULT` log,
   `ext list` shows `[FAULTED]`, panel shows the FAULT banner.
3. App: the toggle turns itself OFF (Is Active notify) — THE assertion.
4. App: re-activating over BLE is rejected (toggle reverts, no crash loop).
5. Serial: `ext select <slot>` clears the fault; `ext list` clean;
   re-activation works again.

Pass: 3 + 4 + 5. Cleanup: hello Crash param back to 0 (the fault path
already reset persisted params to defaults — verify Speed reads 50).

## E2E-07 — Notification-slot leak (#285/#286)

Android's 15-registration cap fails silently (CCC write still succeeds) —
the fw-v2.1.0 "every SMP call times out" regression.

1. From a healthy connection, cycle: open battery/status page (subscribes) →
   navigate away (unsubscribes) × 20.
2. Serial during the whole run: zero `Notify failed: -12` / `No ATT channel`
   lines.
3. After the cycles: trigger an SMP operation (e.g. app reads image list) —
   must complete, not time out.

Pass: 2 + 3. Known flake: a single transient `-12` right at a navigation
boundary that does not recur → note, retry once.

## E2E-08 — Connection-parameter governor (#188/#199/#200)

1. Healthy connection, leave the app idle (no reads) for 20+ s.
2. Serial `bt_state`: governor target SLOW; interval 150–165 ms, latency 2,
   timeout 5 s.
3. Observe 2+ min idle: zero disconnects (`reason 8` = the out-of-spec-32k
   supervision-timeout regression).
4. Interact with the app (open a characteristic-heavy page): governor steps
   up (FAST/MEDIUM), then decays back to SLOW after idle.

Pass: 2 + 3 + 4 from `bt_state` alone (the serial side is fully
instrumented for this).

## E2E-09 — Pairing-overlay timeout (#242/#244)

1. Clear the bond (board side) so pairing is required; start pairing from
   the app but DO NOT enter the passkey; let Android's dialog time out.
2. Serial: `bt_state` returns to ADVERTISING and `anim indicator get` →
   `none` — the overlay must not stay up (the stuck-overlay bug also froze
   shuffle).
3. Then complete a normal `/re-pair` to leave the rig healthy.

Pass: step 2 within the SMP timeout + 5 s. Cleanup: step 3.

## E2E-10 — Dropdown notify at MTU floor (#214)

1. Provision a `.glim` whose filename is 31 chars (host-side over USB MSC,
   reboot to rescan).
2. App: select it from the GLIM dropdown; also `glim select` it from serial
   with the app subscribed.
3. Serial: zero `Notify failed: -12` / `bt_att: No ATT channel for MTU`
   lines; app re-reads the selection list on the (truncated-preview)
   notification and shows the right selection.

Pass: step 3. Cleanup: delete the long-named file, reboot.

---

## Coverage map (issue #333's explicit asks)

| Ask | Scenario |
| --- | --- |
| FW update | E2E-02 (app wizard) + HIL dfu tier (mcumgr path) |
| Extension update | E2E-04 |
| Bootloader update | E2E-03 |
| All accessible animation controls | E2E-05 (+ E2E-06 for Is Active) |

## Graduation path

After one manually-validated agent run of this plan (results posted), wrap
the execution contract in a `/e2e-test` skill so future runs are one command.
Until then this document is the source of truth; keep `plan-version` bumped
on every semantic change so results stay comparable.
