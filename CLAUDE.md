# CLAUDE.md — RGB Sunglasses Project

## Memory policy

**Always use in-repo files for memory.** The devcontainer is rebuilt often, so `~/.claude/` is ephemeral — never store
lasting facts there. Agent knowledge is layered so only what is relevant loads:

| Layer | Loads | Holds |
|---|---|---|
| This file | every session | cross-cutting rules, routing |
| `fw/CLAUDE.md`, `app/CLAUDE.md` | first Read in that subtree | conventions + index of rules files |
| `.claude/rules/*.md` | a Read file matches its `paths:` | code-area detail, numbers, history |
| `.claude/skills/*/` (+ `references/`) | the task matches the skill | procedures, hardware ops |
| `docs/agent-incidents.md` | on demand, by anchor | stories behind rules with no code trigger |

**"Remember" instructions:** when the user says "Remember" (or "Remember that"), record it at once in the most specific
layer — a code-area fact in the `.claude/rules/` file whose `paths:` cover that code (create one and index it in fw/ or
app/ if none does), a procedure in its skill, a cross-cutting rule here. Always-loaded files carry the rule plus a
pointer; numbers, dates and stories go in the chunk. Run `python3 scripts/check-agent-docs.py` after editing an agent doc.
Always use the built-in file tools to edit files.

## Session startup

**Your first output in every new conversation must be the environment status summary table — before any task work, even
when the user opens with a specific request.** A `SessionStart` hook injects `check-hardware` + `check-software` output
as "Environment status (auto-checked at session start)"; render it as a brief table (dev board, J-Link, Android/ADB,
`gh`, …) and call out anything NOT AUTHENTICATED / NOT READY up front. Only run `/check-hardware` / `/check-software`
yourself if that block is missing.

- **Launch agents from the repo root or a worktree root only** — settings, hooks and `.claude/rules/` live at the root;
  subdirectory launches are unsupported.
- Read `fw/CLAUDE.md` / `app/CLAUDE.md` before planning work there. On a macOS host (Mac Mini), read
  `.claude/skills/flash-and-verify/references/macos-host.md`.

## Working with hardware

- **Check presence with `/check-hardware`, never a bare `adb devices` / `lsusb`** — it fixes USB device nodes first, so
  a raw probe can miss a connected device.
- **Never run `/check-hardware` (or anything calling `adb kill-server`) during an app install** — it can kill the
  install; use `lsusb | grep 2fe3` + `fw/scripts/fix-usb-dev-nodes.sh` instead
  (`docs/agent-incidents.md#2026-07-25-check-hardware-killed-an-in-flight-app-install`).
- **Use only the `mcp__serial__*` tools for the board's shell**, never raw Bash on `/dev/ttyACM*`
  (`.claude/skills/flash-and-verify/references/serial-shell.md`).
- Hardware iterations are slow and mistakes can cause damage: follow the pre-flash gates in `/flash-and-verify` §2
  (confirm assumptions in source, confirm Kconfig in `autoconf.h`).

### NEVER write unverified commands or data into hardware parts

**Never send a command, register write, 4CC task, or configuration value to a physical part (I2C/SPI peripheral, PD
controller, charger, sensor, …) based on memory, inference, or pattern-matching.** Recalled datasheet content is a
hallucination until proven otherwise, and a wrong write can permanently wedge a chip. Before ANY write that is not an
established, hardware-proven code path:

1. **Obtain the authoritative source first** — the datasheet / TRM, from the user or checked into the repo
   (`fw/docs/datasheets/`: TPS25750 TRM + datasheet, BQ25792, MX25R6435F). Web summaries, recall, and "the other
   constants look like this" do NOT count.
2. **If the source is not available, STOP and ask the user for it.** Hardware is not a REPL.
3. Cite the doc section for the exact bytes/values written in the code comment.
4. Reads are comparatively safe; writes are the danger. An unused define is NOT evidence.

For a hardware symptom (e.g. wrong current/voltage readings), check `/debug-fw` before any externally-suggested fix.
Why: `docs/agent-incidents.md#2026-07-05-tps25750-go2p-wedge`; power parts: `.claude/rules/fw-power.md`.

### Choosing which phone to use

**Any connected phone is viable.** The phones in `.claude/skills/drive-app/references/phones.md` are the *known* ones,
with their tap recipes and BLE quirks. BLE strictness changes what a pass proves: when a change touches the GATT layout,
say which phone you verified on and what that covers (`docs/agent-incidents.md#2026-08-24-phone-choice-rule-relaxed`).

**NEVER connect an ADB device yourself.** No `adb connect`, `adb disconnect`, `adb pair`, no re-pairing a dropped
wireless session, no switching transport, no walking the user through it. **If nothing is connected, or a device drops
mid-session, say so plainly and stop** — report `adb devices -l`, whether the host answers a ping, whether the port is
refused (`docs/agent-incidents.md#2026-08-26-agent-reconnected-a-dropped-adb-session`).

### NEVER reboot the shared Android phone on your own

**Never `adb reboot` the phone without asking** — it comes back locked and nothing over ADB can unlock it. Rebooting the
*board* is fine. If BLE/ADB seems stuck (e.g. scan error 6), climb: force-stop the app → `adb shell svc bluetooth
disable`/`enable` → `adb shell am force-stop com.android.bluetooth` → reset the board → only then ask the user to
power-cycle the phone (`docs/agent-incidents.md#2026-08-12-bt-stack-registration-leak-on-the-oneplus`).

### BLE pairing — use the `/re-pair` skill; otherwise ask the user for the passkey

The firmware requires `BT_SECURITY_L4`; a fresh pairing prints a 6-digit passkey on the board's serial console for
Android's native dialog. **Re-pair with `/re-pair`**; outside it, **ask the user before entering a passkey via ADB**
(`.claude/skills/re-pair/references/manual-pairing.md`).

## Hardware locking

Agents in separate worktrees share one dev board (+J-Link) and one phone. Before flashing, provisioning, opening
`mcp__serial__*`, or driving the phone (`mcp__execbro__*`/ADB), hold the lock. `hold` is the *only* way to take one —
run it as a long-lived `Monitor` task, then confirm:

```
Monitor(command: "scripts/hw-lock.sh hold board", description: "board hw-lock heartbeat", persistent: true)
```
```bash
timeout 15 bash -c 'until scripts/hw-lock.sh check board >/dev/null 2>&1; do sleep 0.5; done'
```

- **A lock is exclusive for as long as the `hold` task runs — full stop.** No timer or quiet hardware releases it.
  Release by stopping the task (`TaskStop`) or `scripts/hw-lock.sh release board app`; releasing `app` also stops Metro.
- A conflicting `hold` fails at once; `--wait SECONDS` queues FIFO. Re-running `hold` on a lock your own session holds
  adopts it — the recovery move after a lost heartbeat.
- **Release once genuinely done** — hold across a whole build → flash → test loop, but don't squat. You are nudged when
  someone queues behind you; nothing forces a release.
- A `PreToolUse` hook denies `mcp__serial__*`/`mcp__execbro__*` and Bash containing `jlink-flash.sh`,
  `provision-device.sh`, `JLinkExe`, `nrfutil`, `mcumgr`, `west flash`, `adb` or `expo run:android` without the lock.
  **It matches command text** — even a `grep` mentioning them is denied; use the Read/Grep tools.
- Launch the app only via `/launch-app`, never `npx expo run:android` directly. Command surface, Metro/lock lifecycle,
  macOS and enforcement limits: `/hw-lock`.

## Worktree isolation — NEVER touch the main checkout from a worktree

**In a git worktree (`.claude/worktrees/<name>/`), operate ONLY on files under that worktree. Never read, build against,
copy from, edit, or flash artifacts from the main checkout or another worktree.** Every path and `--build-dir` stays in
the worktree; if something is missing there (e.g. `fw/build`), build it there. Both mistakes have happened.

## Git workflow — ALWAYS branch before committing

**Never commit directly to `main`** — branch first, then commit, push, and open a PR via `/submit-pr`.
`.claude/hooks/destructive-guard.sh` denies `git commit` on `main`. Replying to PR review comments with `gh api` has
traps (`-f body=@file` posts the literal path, pending reviews, pagination):
`.claude/skills/submit-pr/references/gh-review-comments.md`.

## Process management — NEVER use pkill

**Never use `pkill` or `killall` in the devcontainer** — they kill container-wide (init, MCP server, VS Code server) and
end the session. Kill by PID (`$!`, or `pgrep` + `kill <pid>`). To restart Metro, stop the `app/scripts/launch-app.sh`
task and relaunch it (`/launch-app`). `destructive-guard.sh` also denies `pkill`/`killall`, `mkfs` and
`reset-project.js`.

## Installing tools

**Add every new tool or dependency to the environment's setup definition** so it survives a rebuild — never an ad-hoc
`apt`/`brew`/`pip install`: `.devcontainer/Dockerfile` or `postCreateCommand` in `.devcontainer/devcontainer.json`
(Linux); `scripts/macos-setup.sh` or `app/scripts/macos-setup.sh` (macOS host, both idempotent).

## Don't rebuild what already exists

**Reimplementing something in the Zephyr/NCS tree needs an extremely strong reason; reimplementing something already in
THIS repo must be flagged to the user before you build it.** Check the SDK first (`zephyr/drivers/`, `zephyr/subsys/`,
the `*_shell.c` files). A strong reason means the stock version cannot do the job and you can say why — not that it is
awkward, or formats output differently, or **that a design decision of yours broke it**: when the argument is "the
existing one does not work here", check whether YOUR change stopped it working
(`docs/agent-incidents.md#2026-08-11-pr-325-reset_cause-module`).

Habits: a warning comment explaining why a stock feature is disabled is a signal to re-examine the design (needing
several is close to proof); and compare "my version" against "stock version alone", never "stock added on top of mine".

## Repository layout

| Directory | Contents |
| --- | --- |
| `fw/` | Zephyr RTOS firmware (nRF5340) — `fw/CLAUDE.md` |
| `app/` | React Native companion app (Expo) — `app/CLAUDE.md` |
| `.devcontainer/` | Devcontainer definition, `check-hardware.sh`, `check-software.sh` |
| `.claude/` | `rules/` (path-scoped), `skills/`, `agents/`, `hooks/`, `settings.json` |
| `scripts/` | Host tooling: `hw-lock.sh`, `re-pair.sh`, `pr-watch.sh`, `check-agent-docs.py`, `macos-setup.sh`, env scripts, `tests/` |
| `docs/` | `agent-incidents.md`, `plans/` (dated design plans) |
| `extensions/` | Community extension registry |

## Task routing

This is the project's **single** routing table — other docs link here, never copy it:

| Task | Skill |
| ---- | ----- |
| Add or modify a built-in animation | /add-animation |
| Add or change a GATT service/characteristic (+ app UI) | /add-gatt-characteristic |
| Write or modify a loadable `.llext` extension (in-repo) | /add-extension |
| Standalone extension repo / rgbx-sdk / community registry | `fw/docs/standalone-extension-repos.md` + `extensions/README.md` (SDK: `fw/sdk/`) |
| Build the firmware (proto0, incremental) | /build-proto0 |
| Run the firmware tests + coverage | /test-fw |
| Add or fix a firmware test (native_sim/Twister) | /add-fw-test |
| Run or extend the on-device (HIL) suite | `fw/tests_device/README.md` (`fw/scripts/run-device-tests.sh`, `fw/docs/on-device-testing.md`) |
| App+device E2E test run (AI-driven) | /e2e-test (`fw/docs/e2e-test-plan.md`) |
| Debug a firmware symptom | /debug-fw |
| Debug a device↔app BLE symptom | /debug-ble |
| Re-pair the phone to the board | /re-pair |
| Record a real audio + IMU capture as a sim scenario | /capture-scenario |
| Validate app changes without a phone | /validate-app |
| Launch / deploy the companion app on a phone | /launch-app |
| Drive the app's UI on the phone | /drive-app |
| Firmware OTA through the app end to end (/submit-pr step 5a) | /ota-via-app |
| Memory / FLASH / RAM work | /rom-ram-budget |
| Flash + on-device verification | /flash-and-verify |
| Provision a board's NAND (FAT, GLIM assets, extensions) | /provision-device |
| Flash / recover without a J-Link (MCUmgr serial, MCUboot DFU) | `fw/scripts/mcumgr-flash.sh` + `fw/docs/flashing-without-jlink.md` |
| Hold / release / inspect hardware locks | /hw-lock |
| Fresh worktree/session orientation | /worktree-setup |
| Prove a change actually works | /verify |
| Pre-PR gate | /submit-pr |
| Watch GitHub PRs and auto-review them | /pr-review-watch |
| Cut a release | /release |

Four things sound alike: a **built-in C++ animation** compiled into firmware = /add-animation; an in-repo **loadable
`.llext` extension** = /add-extension; a **community extension** (same `.llext`, built in a standalone repo against the
released `rgbx-sdk`) = the standalone row; a **`.glim` asset file** = `fw/src/storage/GLIM_FORMAT.md` + the `fw/tools/`
converters.
