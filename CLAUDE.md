# CLAUDE.md — RGB Sunglasses Project

## Memory policy

**Always use in-repo files for memory.** This devcontainer is rebuilt frequently, so `~/.claude/` is ephemeral and must never be used to store facts that need to survive across sessions. Record everything worth remembering in:

- This file (cross-cutting agent behavior, project-wide facts)
- `fw/CLAUDE.md` (firmware-specific guidance)
- `app/CLAUDE.md` (React Native app guidance)
- Other committed files in the repo

Never write to `~/.claude/projects/` or any other `~/.claude/` path for persistent notes.

## Working with hardware

**Never run `/check-hardware` (or anything else that calls `adb kill-server`) while an app deploy/install is in flight.** The skill restarts the adb server as part of its phone probe, which kills any in-progress `adb install` — observed 2026-07-25: an `expo run:android` install failed with a bare exit-1 because check-hardware was run right after a firmware flash while the install was streaming. Board-side re-enumeration checks after a flash can use `lsusb | grep 2fe3` + `fix-usb-dev-nodes.sh` directly when Metro/expo is mid-deploy.

**Always check for device presence with the `/check-hardware` skill (`.devcontainer/scripts/check-hardware.sh`), never a bare `adb devices` / `lsusb`.** The skill applies USB device-node fixes (re-triggers enumeration/authorization) as part of the check, so a device that a raw `adb devices` reports as NOT CONNECTED can show up correctly once check-hardware runs. Do not conclude "no phone/board attached" from a bare `adb devices` — run check-hardware first (observed 2026-07-19: `adb devices` empty, check-hardware then reported the phone CONNECTED over USB).

Hardware iterations are slow and mistakes can cause damage. Before flashing anything:

- Read the relevant source code to confirm assumptions (Kconfig deps, handler logic, buffer sizes)
- Verify changes in `build/fw/zephyr/include/generated/zephyr/autoconf.h` before uploading
- Don't rely on web search results for Kconfig symbol names — check the actual NCS source under `/root/ncs/v3.1.1/` (devcontainer) / `~/ncs/v3.1.1/` (macOS host)
- Verify memory-accounting claims against the linker map (`build/fw/zephyr/zephyr.map`) before proposing size/config changes — e.g. `.noinit` buffers (like the llext heap) ARE counted in the linker's RAM percentage, and secondary reports (footprint scripts) use different accounting than what governs link success
- **A before/after diff of the build's FLASH/RAM totals can have the wrong SIGN.** Under `CONFIG_USERSPACE` the gperf-generated `kobject_data` section is sized by a perfect hash over kernel-object *addresses*, so any change that shifts the layout resizes it by kilobytes in either direction, unrelated to what the change actually costs. Measured 2026-08-11 adding two GATT characteristics (issue #148): the totals moved −2,720 B FLASH / −5,216 B RAM, which reads as a saving, while the change itself cost **+2,904 B FLASH / +412 B RAM** — `kobject_data` had simply hashed 5,632 B smaller. Attribute cost from the map's per-output-section deltas (`text`/`rodata`/`datas`/`bss`), and treat a `kobject_data` delta as noise to be reported separately, never as part of the change's cost. Same family as the unexplained `kMaxAttrs` nonlinearity documented at `fw/src/extensions/extension_bt.cpp:48`.

### NEVER write unverified commands or data into hardware parts

**Never send a command, register write, 4CC task, or configuration value to a physical
part (I2C/SPI peripheral, PD controller, charger, sensor, etc.) based on memory,
inference, or pattern-matching. LLM-recalled datasheet content is a hallucination until
proven otherwise, and a wrong write can permanently damage or wedge a chip.**

Before ANY write to a hardware part that is not already an established, in-repo,
hardware-proven code path:

1. **Obtain the authoritative source first** — the actual datasheet / technical
   reference manual (a PDF or excerpt provided by the user, or a doc checked into the
   repo). Web search summaries, training-data recall, and "the other constants look
   like this" pattern-matching do NOT count.
2. **If the source is not available, STOP and ask the user for it.** Do not "try
   something plausible and see" — hardware is not a REPL. The user would rather be
   asked than have a part bricked.
3. Cite the doc section for the exact bytes/values being written in the code comment,
   so the next reader can re-verify.
4. Reads are comparatively safe; writes are the danger. A define existing unused in
   the codebase is NOT evidence it is correct — unused code was never
   hardware-validated.

Before implementing any externally-suggested hardware fix for a symptom (e.g. wrong
current/voltage readings), check `.claude/skills/debug-fw/` first — known root causes
for these symptoms are catalogued there (BQ25792 sign extension, PR #106; TPS25750
I2Cm bridge race, PR #111).

This rule exists because of a real incident (2026-07-05): unverified TPS25750 4CC
commands ("GO2P"/"Go2P" — spelling and semantics asserted from memory, not from the
TRM) were written to CMD1 on live hardware while attempting to force a patch
re-download, and the part ended up in a broken state. The correct move at step zero
was: "I don't have the TPS25750 host-interface TRM — please provide it before I write
anything to this chip."

The TRM (and the TPS25750/BQ25792 datasheets) are now checked in under
`fw/docs/datasheets/` — that is the authoritative source this rule demands. GO2P
itself has since been implemented the sanctioned way (user-commissioned 2026-07-17,
cited to TRM SLVUC05A Table 3-12): `tps25750_go2p()` + the `power pd go2p` shell
command, which refuses to run without a battery present (the 2026-07-05 wedge was
likely aggravated by running VBUS-only when GO2P dropped the PD PHY). It exists to
exercise the runtime PTCH-wedge recovery path — see `/debug-fw`'s symptom table.

### NEVER switch phones on your own

**The OnePlus 9 Pro (LE2125) is the phone to use. If two phones are reachable over
ADB, that is not an invitation to pick one — never switch to the Pixel (or any other
device) automatically.** Instructed 2026-08-12 after an agent found the OnePlus
apparently absent, connected the Pixel instead, and started deploying to it.

If the OnePlus looks unavailable — not in `adb devices`, screen locked, app not
installed — say so and ask. Do not treat "the other phone answers" as the fix. The
tempting rationalisation to avoid: the Pixel has a spec-compliant BLE stack and so
avoids the OnePlus's forget-and-re-pair dance after a GATT-changing reflash — that
is a reason the Pixel is *easier*, not a reason it is the right device, and
`/re-pair` exists precisely to make the OnePlus path routine. Verification done on
the wrong phone also proves the wrong thing: the OnePlus is the strict device, so a
result from the tolerant one does not carry over.

### NEVER reboot the shared Android phone on your own

**Never run `adb reboot` (or any full OS-level reboot) against the shared test
phone without asking the user first.** Rebooting the dev board is fine and
routine (it re-enumerates over USB automatically); rebooting the phone is not
— it comes back up locked, and unlocking a phone's screen is not something
`adb`/`execbro` can do (no ADB command enters a PIN/pattern/biometric), so a
self-triggered phone reboot strands the session until the user physically
walks over and unlocks it by hand.

If BLE/ADB connectivity to the phone seems stuck (e.g. Android's BLE scan
returns `SCAN_FAILED_APPLICATION_REGISTRATION_FAILED` / error code 6 from a
stale scan-client registration), prefer lighter, reversible recovery steps
first — `adb shell svc bluetooth disable` then `enable` to reset just the
Bluetooth stack, re-navigating the app's screen to restart a scan, or
resetting the *board* (not the phone) if a stale GATT link is suspected. Only
ask the user to power-cycle the phone themselves if those don't resolve it —
never do it unilaterally via `adb reboot`.

**When `svc bluetooth disable`/`enable` does not clear error 6, restart the
Bluetooth stack PROCESS instead: `adb shell am force-stop com.android.bluetooth`**
(it restarts itself; `settings get global bluetooth_on` still reads 1 afterwards).
Measured 2026-08-12 on the OnePlus: four `svc` cycles left every scan failing with
registration error 6, and one force-stop of the stack process fixed it on the next
app launch. Force-stop the app first either way — its own registrations are part of
what leaks. This is strictly lighter than the phone reboot the rule above forbids,
so it belongs in the ladder before ever asking the user.

Two things that will otherwise cost an hour on that phone:

- **`pm grant` is blocked on OxygenOS** (`SecurityException: neither user 2000 nor
  current process has GRANT_RUNTIME_PERMISSIONS`), so the pre-grant recipe in
  `app/CLAUDE.md` does not work there. Grant through the UI instead:
  `adb shell am start -a android.settings.APPLICATION_DETAILS_SETTINGS -d package:<pkg>`
  → Permissions → Location → "Allow only while using the app".
- **Every board reboot needs a re-pair on this phone**, not just a GATT-changing
  one — a plain reflash-and-reset wedges the bonded reconnect at `ATT MTU: 23`. Budget
  for `/re-pair` after each flash, and expect its automated forget to need the
  halt-the-board recipe (see `app/CLAUDE.md`); the forget only succeeds while the
  board is unreachable.

### BLE pairing — use the `/re-pair` skill; otherwise ask the user for the passkey

The firmware requires `BT_SECURITY_L4` (LE Secure Connections + bonding). On a
fresh pairing (board recently unpaired, or its bond info was cleared), the
serial console prints something like:

```
[00:23:51.161,041] <inf> bluetooth: Passkey for D0:49:7C:17:7B:E1 (public): 123456
[00:23:51.161,560] <inf> bluetooth: Peer needs to enter a pin code to pair
```

This is the firmware's own `passkey_display` auth callback (`fw/src/bluetooth.cpp`,
IO capability = Display-only, no `passkey_entry`/`passkey_confirm`/`pairing_confirm`
registered) — the phone's Android BLE stack shows a native "Enter pairing code"
system dialog (not part of the companion app's own UI, so `mcp__execbro__android_screenshot`
won't necessarily surface it as an app screen — check for it explicitly) expecting
that exact 6-digit code typed in and submitted.

**The sanctioned way to (re-)pair is `scripts/re-pair.sh` / the `/re-pair` skill**
(user-commissioned 2026-07-11): it forgets the stale bond and re-pairs hands-off, with
a local autoresponder that reads the passkey off the UART and types it into Android's
dialog fast enough to beat the pairing timeout — the exact read-and-enter flow this rule
used to forbid, now packaged as an auditable script that self-gates on the board + app
locks. Use it (see `/debug-ble` for when the OnePlus stale-GATT split-brain needs it).

**Outside that script, still stop and ask the user before entering a passkey via ADB.**
Ad-hoc `adb shell input text`/`mcp__execbro__android_input_text` of a passkey you scraped
by hand remains off-limits without the user's go-ahead — this is BLE pairing state on the
one shared physical phone, same spirit as the phone-reboot rule above. The difference is
that `/re-pair` *is* that go-ahead, standardized.

## Hardware locking

Multiple agents, each in its own worktree, share one physical dev board
(+J-Link) and one physical Android phone. Before flashing, provisioning,
opening an `mcp__serial__*` connection to the board, or driving the phone via
`mcp__execbro__*`/ADB, hold the relevant lock. `hold` is the *only* way to
take a lock — there is no bare "acquire and forget." Launch it as a
long-lived task via the `Monitor` tool, then confirm before touching
hardware:

```
Monitor(command: "scripts/hw-lock.sh hold board", description: "board hw-lock heartbeat", persistent: true)
```
```bash
timeout 15 bash -c 'until scripts/hw-lock.sh check board >/dev/null 2>&1; do sleep 0.5; done'
```

When done, either stop the `hold` task (`TaskStop` — its own exit trap
releases automatically) or run `scripts/hw-lock.sh release board app`
yourself. **Holding a lock means exclusive access for as long as you keep the
`hold` task running — full stop.** It's never released by a timer or by a
hardware surface going quiet (e.g. the J-Link de-enumerating mid-flash is
normal and is never evidence the lock is safe to release); the only things
that end a hold are you stopping it, or the process dying (crash, kill,
container restart), which the stale-pid reclaim already handles safely on
the next attempt.

By default a conflicting `hold` fails immediately. Pass `--wait SECONDS`
(e.g. `hold board --wait 300`) to queue instead of bailing out — a real
FIFO queue (ticket per resource, oldest arrival goes first), not independent
agents racing each other when the resource frees up, and still strictly
all-or-nothing on every attempt so waiting agents can never deadlock on each
other.

If **your own session already holds** the resource and you run `hold` again
(e.g. after a heartbeat/`Monitor` task died or was lost across a context
reset), the new `hold` **adopts** the lock — it takes over as the tracked
heartbeat and reports success immediately, rather than refusing or waiting on
itself. So the recovery move after any board-lock heartbeat failure is simply
to **re-run `hold`**: if a prior in-session hold is still alive it's adopted, if
it died cleanly the lock was already released, and if it died hard the stale-pid
reclaim clears it first — every case ends with you holding a live heartbeat. A
`--wait` on a lock your own session holds is never futile now (it can't be — the
adopt path returns before queueing). A **different** session's hold still
conflicts exactly as before.

A `PreToolUse` hook (`.claude/hooks/hw-lock-guard.sh`) auto-denies every
`mcp__serial__*`/`mcp__execbro__*` call and known hardware-touching Bash
commands (`jlink-flash.sh`, `provision-device.sh`, `JLinkExe`, `mcumgr`,
`west flash`, `adb`, `expo run:android`) unless the relevant lock is held —
this is a backstop, not a substitute for holding proactively, since a denial
interrupts whatever flow triggered it. `fw/scripts/jlink-flash.sh` and
`fw/scripts/provision-device.sh` hard-refuse to run without the `board` lock
on their own, independent of the hook, so they're covered even outside a
Claude Code session — and neither ever acquires the lock itself, only checks
it. Launch the companion app via `app/scripts/launch-app.sh` (never call
`npx expo run:android` directly) — it follows the same check-only pattern
now: it refuses to run unless `app` is already held, and it no longer
acquires the lock itself, but the relationship isn't fully symmetric.
Stopping the `hold` task (or `release app`, same-session) now also stops
Metro if it's still running — releasing the lock guarantees Metro has
quit. Metro stopping or crashing on its own, though, still does not release
the lock — you still manage that side yourself. (A human force-releasing a
*different* session's still-live lock does not kill that session's Metro —
only same-session release does; the acquire-time cleanup below handles it
instead.) If a Metro/expo process is still running from an earlier, now-dead
session when `app` is next held, that hold kills it automatically before
considering itself established.

While holding a lock, if another agent is queued waiting for it, you'll be
nudged three ways: an `additionalContext` reminder on your next hardware tool
call, the same on your next turn (`UserPromptSubmit` hook), and — the one
that reaches you even fully idle — a notice `hold` itself prints, delivered
via the `Monitor` task's event stream. None of these ever force a release;
they're purely advisory.

**Release once you're genuinely done — not preemptively, and not so late
you're just squatting on it.** Don't release and re-acquire between steps of
one ongoing task you expect to repeat (e.g. a build → flash → test → build →
flash → test iteration loop) — hold across the whole cycle; releasing
between passes you're about to repeat just adds a race for zero benefit. But
don't keep holding "just in case" once the task that needed the resource is
actually finished — the waiter-notice above tells you exactly when someone
else needs it, which is the right moment to wrap up, not something to guess
at preemptively.

See `.claude/skills/hw-lock/SKILL.md` for the full command surface (status,
`--steal`, `--force`, `release --all`) and known enforcement limitations.

Locking works on a macOS host too: `hw-lock.sh` re-execs itself into Homebrew
bash ≥ 4 (installed by `scripts/macos-setup.sh`) when invoked under the stock
macOS bash 3.2. Locks are per-host (stored under the repo's `$GIT_COMMON_DIR`),
which is correct: the physical hardware is attached to exactly one host at a
time, and agents on that host contend with each other, not with agents
elsewhere.

Note the hook's Bash matcher keys on command **text**: even read-only commands
containing the literal tokens `adb`/`mcumgr`/`JLinkExe` (e.g. a `grep` for them)
are denied without the lock — use the Read/Grep tools or avoid the tokens
(see /worktree-setup).

## "Remember" instructions

When the user says "Remember" (or "Remember that"), update the appropriate CLAUDE.md file immediately with the information. Prefer the root `CLAUDE.md` for cross-cutting rules and `fw/CLAUDE.md` for firmware-specific facts.

## Worktree isolation — NEVER touch the main checkout from a worktree

**When working in a git worktree (`.claude/worktrees/<name>/`), operate ONLY on files under that worktree root. Never read, build against, copy from, edit, or flash artifacts from the main checkout at `/workspaces/rgb-sunglasses` (or any other worktree).** Every path you Read/Write/Edit and every `--build-dir`/artifact reference must stay under the current worktree. If something you need doesn't exist in the worktree yet (e.g. `fw/build`), **create/build it here** — do not reach into the main repo's copy. Reading the main checkout's build, docs, or source from a worktree gives stale/wrong results and silently crosses branches. (This is a hard rule from real mistakes: editing main-checkout files while the branch lived in the worktree, and trying to flash the main checkout's build from a worktree.)

## Git workflow — ALWAYS branch before committing

**Never commit directly to `main`.** Always create a feature branch first (`git checkout -b <branch-name>`), then commit, push the branch, and open a PR. Do this before editing any files if possible, but at minimum before the first `git commit`. A `PreToolUse` hook (`.claude/hooks/destructive-guard.sh`) denies `git commit` while on `main`, so branch creation must come first.

### GitHub PR review comments via `gh api`

- While this account has a **pending (draft) review** on a PR, the API rejects ALL new review comments from it with 422 "user can only have one pending review per pull request" — both `POST /pulls/<n>/reviews` and standalone `POST /pulls/<n>/comments` (standalone line comments are single-comment reviews internally). Never touch or submit the user's pending review; fall back to regular PR comments (`gh pr comment`) with `https://github.com/<owner>/<repo>/blob/<sha>/<path>#L<line>` permalinks, which render the referenced snippet inline.
- Once a review is submitted, reply to its line-comment threads with `gh api repos/<owner>/<repo>/pulls/<n>/comments/<comment_id>/replies -f body=...`.
- **`-f body=@file` does NOT read the file — it posts the literal string `@file`.** `-f`/`--raw-field` is verbatim; only `-F`/`--field` interprets a leading `@` as a file path. Writing a long comment to a temp file and passing `-f body=@/tmp/c1.md` (a natural-looking move, and how `curl` behaves) silently publishes a comment whose entire body is `@/tmp/c1.md`. Observed 2026-08-15 on PR #377: five review comments posted this way, all stale on GitHub until repaired. Prefer the unambiguous form, which works regardless of flag semantics:

  ```bash
  gh api -X POST repos/<owner>/<repo>/pulls/<n>/comments -f body="$(cat /tmp/c1.md)" ...
  ```

  Note the temp file is still the right way to carry markdown — heredocs and inline quoting mangle backticks and `$` in code snippets. It is only the `@` hand-off that is broken.
- **Always verify a posted comment round-trips.** The POST returns 201 with a valid comment object either way, so the failure is invisible without an explicit check. After posting, re-read the bodies and assert none is a bare `@path`:

  ```bash
  gh api --paginate repos/<owner>/<repo>/pulls/<n>/comments \
    --jq '.[] | select(.body | test("^\\s*@\\S+\\s*$")) | "STALE \(.id) \(.body)"'
  ```

  Repair in place with `gh api -X PATCH repos/<owner>/<repo>/pulls/comments/<comment_id> -f body="$(cat …)"` — no need to delete and repost, which would lose thread replies.
- Comment listing endpoints **paginate at 30**. A PR with several review rounds silently truncates, so a "did my comment land?" check without `--paginate` can report a false negative.

## Process management — NEVER use pkill

**Never use `pkill` or `killall` inside the devcontainer.** These commands kill processes across the entire container (including the container init, the MCP server, and the VS Code server), which crashes the devcontainer and terminates the session. To stop a background process, use its PID from `$!` or find it with `pgrep` and send a targeted `kill <pid>`. To restart Metro/Expo, just launch a new `npx expo run:android --device <device name> --app-id com.autom8ed.rgbsunglassesapp.dev` — it starts a fresh Metro instance. A `PreToolUse` hook (`.claude/hooks/destructive-guard.sh`) now hard-denies `pkill`/`killall` (and `mkfs`, `reset-project.js`, and commits on `main`) as a backstop — the rule stands regardless.

## Installing tools

When installing any new CLI tool or dependency, **always add it to the environment's setup definition** so it is available to all users after a rebuild/re-run — never ad-hoc `apt install`/`brew install`/`pip install` commands that only affect the current instance:

- Linux devcontainer: `.devcontainer/Dockerfile` or `postCreateCommand` in `.devcontainer/devcontainer.json`
- macOS host (Mac Mini): `scripts/macos-setup.sh` (firmware + agent tooling) or `app/scripts/macos-setup.sh` (iOS app toolchain) — both idempotent, safe to re-run

## Don't rebuild what already exists

**Reimplementing something that already exists in the Zephyr/NCS tree requires an
extremely strong reason. Reimplementing something that already exists in THIS repo must
always be flagged to the user for review before you build it.**

The SDK is the first place to look, not the fallback. Before writing a driver, a shell
command, a decoder, a state machine or a utility, check whether Zephyr already ships one —
`zephyr/drivers/`, `zephyr/subsys/`, and the `*_shell.c` files in particular. A stock
implementation is maintained upstream, is already documented, already has more surface
than you will write, and does not cost review time.

"An extremely strong reason" means the stock version cannot do the job, and you can say
concretely why. It does NOT mean:

- the stock version is slightly awkward to call;
- you would like different output formatting;
- **a design decision you made yourself broke it.** This is the trap. Real incident
  (2026-08-11, PR #325): a custom `reset_cause` module was written with its own copy of
  Zephyr's `RESET_*` name table, justified on the grounds that
  `CONFIG_HWINFO_SHELL`'s `hwinfo reset_cause show` "would read 0 and therefore lie". It
  would only read 0 because that same new module cleared `RESETREAS` at boot. The
  justification was a consequence of the thing being justified. Removing the clear made
  the built-in work correctly and the custom module unnecessary.

  When the argument for building something is "the existing one does not work here",
  check whether YOUR change is what stopped it working.

Two habits that catch this early:

- When you find yourself writing a warning comment explaining why a stock feature is
  disabled or misleading, treat that as a signal to re-examine the design rather than to
  write the comment. Needing several such comments is close to proof.
- Compare the right two options. Measuring "stock feature added ON TOP of my version"
  answers nothing; the comparison that decides it is "my version" versus "stock version
  alone".

## Session startup

**Your first output in every new conversation must be the environment status summary table — before any task work, even when the user opens with a specific request.** A `SessionStart` hook (configured in `.claude/settings.json`) already runs `check-hardware` and `check-software` automatically and injects their output into context as "Environment status (auto-checked at session start)", so you normally do **not** need to re-run the skills — just read that injected block and surface it. Only run `/check-hardware` / `/check-software` yourself if that injected block is missing.

Render the results as a brief markdown table (hardware: dev board, J-Link, Android/ADB; software: `gh` and any other tools). If any tool is NOT AUTHENTICATED or NOT READY, call it out explicitly in that first message — **don't wait until it blocks a later step** (e.g. `gh` auth blocks PR creation). Having the data in context is not enough; the user needs to see it up front.

Before working on any subsystem, **read its CLAUDE.md first** — those files are the project's persistent memory and contain critical workflow rules (correct commands, known pitfalls, launch procedures) that are not derivable from the code alone. Skipping them leads to doing the wrong thing (e.g. launching the Android app incorrectly). Specifically:

- About to touch firmware (`fw/`)? Read `fw/CLAUDE.md` first.
- About to touch the app (`app/`)? Read `app/CLAUDE.md` first.

## Repository layout

| Directory           | Contents                                                                                 |
| ------------------- | ---------------------------------------------------------------------------------------- |
| `fw/`               | Zephyr RTOS firmware (nRF5340). See `fw/CLAUDE.md` for build/test commands.              |
| `app/`              | React Native companion app (Expo). See `app/CLAUDE.md` for architecture and agent notes. |
| `.devcontainer/`    | Devcontainer definition and setup scripts.                                               |
| `.claude/skills/`   | Project skills (slash commands).                                                         |
| `.claude/hooks/`    | Claude Code hooks (e.g. the hardware-lock `PreToolUse` guard).                           |
| `scripts/`          | Cross-cutting host tooling shared by all subsystems — `hw-lock.sh`, the multi-agent hardware-lock coordinator (see "Hardware locking" above), and `pr-watch.sh`, the GitHub PR watcher behind /pr-review-watch. |

## Task routing

This is the project's **single** routing table — other docs link here, never copy it. Match your task to a skill before improvising:

| Task | Skill |
| ---- | ----- |
| Add or modify a built-in animation | /add-animation |
| Add or change a GATT service/characteristic (+ app UI) | /add-gatt-characteristic |
| Write or modify a loadable `.llext` extension (in-repo) | /add-extension |
| Standalone extension repo / rgbx-sdk / community registry | `fw/docs/standalone-extension-repos.md` + `extensions/README.md` (SDK code: `fw/sdk/`) |
| Add or fix a firmware test (native_sim/Twister) | /add-fw-test |
| Run or extend the on-device (HIL) test suite | `fw/tests_device/README.md` (runner: `fw/scripts/run-device-tests.sh`; architecture: `fw/docs/on-device-testing.md`) |
| App+device E2E test run (AI-driven, phone + board) | /e2e-test (executes `fw/docs/e2e-test-plan.md`) |
| Debug a firmware symptom | /debug-fw |
| Debug a device↔app BLE symptom | /debug-ble |
| Record a real audio + IMU capture as a sim scenario | /capture-scenario |
| Validate app changes without a phone | /validate-app |
| Drive the app's UI on the physical phone (tap, wait for a screen change) | /drive-app |
| Memory / FLASH / RAM work | /rom-ram-budget |
| Flash + on-device verification | /flash-and-verify |
| Flash / recover firmware without a J-Link (MCUmgr serial, MCUboot DFU) | `fw/scripts/mcumgr-flash.sh` + `fw/docs/flashing-without-jlink.md` |
| Fresh worktree/session orientation | /worktree-setup |
| Prove a change actually works | /verify |
| Pre-PR gate | /submit-pr |
| Watch GitHub for new PRs/pushes and auto-review them in parallel | /pr-review-watch |
| Cut a release | /release |

Four things sound alike — don't mix them up: a **built-in C++ animation** compiled into firmware = /add-animation; an in-repo **loadable `.llext` extension** = /add-extension; a **community extension** (same `.llext` on the device, but developed in a standalone repo against the released `rgbx-sdk`, never the in-repo EDK path) = the standalone-extension row above; a **`.glim` asset file** (stored animation data) = `fw/src/storage/GLIM_FORMAT.md` + the `fw/tools/` converters (see `fw/CLAUDE.md`).
