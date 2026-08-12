---
name: e2e-test
description: Execute the AI-agent app+device end-to-end test plan on the physical phone + board — pairing, OTA, extension management, all animation controls, and the BLE-only regressions that the shell-driven HIL suite cannot reach. Use for a full E2E validation pass, or to run a single scenario from the plan.
allowed-tools: Bash, Skill, mcp__execbro__scan_metro, mcp__execbro__get_screen_state, mcp__execbro__android_screenshot, mcp__execbro__tap, mcp__execbro__android_input_text, mcp__execbro__android_key_event, mcp__execbro__dismiss_keyboard
---

# App + device end-to-end test run

The scenario catalogue is **`fw/docs/e2e-test-plan.md`** (carries `plan-version:`).
This skill is the execution contract for running it — what to hold, in what order,
how to cross-check, and how to record results. It complements the shell-driven HIL
suite (`fw/tests_device/`, `/`-runnable via `fw/scripts/run-device-tests.sh`): that
covers everything reachable over the UART; this covers everything that needs the
companion app over BLE.

Deliverable of issue #333 part (b). First validated run: tracking issue
"E2E run results — issue #333" (#352), plan-version 1.0.0.

## Preconditions (in order)

1. **Hold BOTH `board` and `app` locks** for the whole run:
   ```
   Monitor(command: "scripts/hw-lock.sh hold board app", persistent: true)
   ```
   ```bash
   timeout 15 bash -c 'until scripts/hw-lock.sh check board >/dev/null 2>&1 && scripts/hw-lock.sh check app >/dev/null 2>&1; do sleep 0.5; done'
   ```
2. **Board on a known image + app installed.** `/check-hardware` (board + Android
   both present). If the board isn't provisioned, `/provision-device` first —
   several scenarios assume the baseline GLIM/extension set.
3. **App + Metro up:** `app/scripts/launch-app.sh` (fresh worktree needs `npm ci`
   in `app/` first). **Then `scan_metro`** and confirm a CDP target appears — do
   NOT trust "app installed"; the JS runtime must register an inspector target or
   every execbro tool fails `[NO METRO]`. If `/json/list` is `[]`, the app is
   installed but its runtime never attached (OxygenOS backgrounded it) — foreground
   it and re-scan, don't rebuild.

## Conventions (learned the expensive way — do not deviate)

- **`/drive-app` targeting rules apply to every tap.** `tap(testID=)` first,
  `tap(text=, strategy="accessibility")` second, coordinates from
  `get_screen_state` third. Never `tap(component=)` / `index=` (whole nav stack
  stays mounted). Never scale screenshot coordinates.
- **The cross-check is the point.** Every app write is verified over the serial
  shell, not just by the app's own re-read: app write → serial read → app re-read.
  The three transports coexist fine (BLE app link + USB shell + J-Link).
  Serial handles: `fw/scripts/tty-bridge.py` (raw shell), `ext param <slot> <idx>`,
  `ext shuffle <slot>`, `glim get_selected`, `bt_state`, `ext list`, `ext faults`.
- **`get_screen_state` reports a `TextInput` value as `empty` even when set** —
  it does not surface controlled-input text. Verify writes over serial, never by
  reading the state dump back.
- **Text commit is the Android return key** (`android_key_event ENTER` after
  `android_input_text ... replace:true`), not an on-blur event (issue #149).
- **The Controls back header** matches fiber label `"Back to Controls"` but OCR
  reads `"‹ Controls"` — target it by coordinate or fiber label, not OCR text.
- **Never log-poll during an SMP upload** and never `android_screenshot` in a loop
  (`/drive-app` explains both).
- **Pairing is `/re-pair` only** — never hand-enter a passkey. Get the exact
  advertised name first (`bt_state` `BT device name:` or
  `adb shell dumpsys bluetooth_manager | grep "RGB Sunglasses"`).

## Execution order

Run **connection-preserving** scenarios before **destructive** ones so one run
doesn't invalidate the next:

1. E2E-01 pairing (`/re-pair`) — establishes the link; also the marquee scenario.
2. Non-destructive, connection-preserving: **E2E-05** (controls matrix),
   **E2E-06** (Is Active + fault recovery), **E2E-07** (notification-slot leak),
   **E2E-08** (governor). These leave the board/bond intact.
3. Destructive / staging-heavy, each ideally its own pass: **E2E-02** (OTA —
   permanently rewrites, OVERWRITE_ONLY), **E2E-04** (ext mgmt — reboots),
   **E2E-10** (dropdown MTU — plant a 31-char `.glim` + reboot), and **LAST**
   **E2E-09** (pairing-overlay timeout — drops the bond; follow with `/re-pair`).
4. **E2E-03 (bootloader) is human-supervised** — never run unattended (brick risk).

`ext faults`/coredump artifacts a fault scenario produces must be cleaned up
(`ext faults clear`, `fs rm /NAND:/coredump/*`) so the board is left pristine.

## Recording results

Fill one results object per run (schema in `fw/docs/e2e-test-plan.md`): per
scenario `status: pass|fail|skip|flake|deferred` + serial/screenshot evidence.
Post it as a comment on the **"E2E run results — issue #333"** tracking issue
(#352) — keep the repo clean, keep history greppable. Cite the `plan-version`.
`flake` = failed then passed on retry with a known-flake signature; anything that
fails twice is `fail`.

## Single scenario

To run just one (e.g. after a fix): satisfy the preconditions, then execute that
scenario's steps from the plan. For a destructive one, still restore state
afterwards (`/re-pair` after E2E-09; remove any planted files + reboot).
