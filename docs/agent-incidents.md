# Agent incident log

The dated stories behind rules in the agent docs (`CLAUDE.md`, `fw/CLAUDE.md`,
`app/CLAUDE.md`, `.claude/rules/`). The rule itself lives in those files; this page keeps
the full account of *why*, so the always-loaded files can stay short. Each section is
linked from the rule it justifies by its anchor (e.g.
`docs/agent-incidents.md#2026-07-05-tps25750-go2p-wedge`).

Add a section here when a rule is born from an incident that has no natural home in a
path-scoped rule or skill. Keep headings ASCII so the anchors are predictable.

## 2026-07-05 TPS25750 GO2P wedge

Rule: root `CLAUDE.md` "NEVER write unverified commands or data into hardware parts".

Unverified TPS25750 4CC commands ("GO2P"/"Go2P" — spelling and semantics asserted from
memory, not from the TRM) were written to CMD1 on live hardware while attempting to force a
patch re-download, and the part ended up in a broken state. The correct move at step zero
was: "I don't have the TPS25750 host-interface TRM — please provide it before I write
anything to this chip."

The TRM (and the TPS25750/BQ25792 and MX25R6435F datasheets) are now checked in under
`fw/docs/datasheets/` — that is the authoritative source the rule demands. GO2P itself has
since been implemented the sanctioned way (user-commissioned 2026-07-17, cited to TRM
SLVUC05A Table 3-12): `tps25750_go2p()` + the `power pd go2p` shell command, which refuses
to run without a battery present (the 2026-07-05 wedge was likely aggravated by running
VBUS-only when GO2P dropped the PD PHY). It exists to exercise the runtime PTCH-wedge
recovery path — see `/debug-fw`'s symptom table and `.claude/rules/fw-power.md`.

## 2026-07-19 bare adb devices reported no phone

Rule: root `CLAUDE.md` "Working with hardware" — check presence with `/check-hardware`,
never a bare `adb devices` / `lsusb`.

`adb devices` came back empty; running check-hardware then reported the phone CONNECTED
over USB. The skill applies USB device-node fixes (re-triggers enumeration/authorization)
as part of the check, so a device a raw `adb devices` reports as NOT CONNECTED can show up
correctly once check-hardware runs.

## 2026-07-25 check-hardware killed an in-flight app install

Rule: root `CLAUDE.md` "Working with hardware" — never run `/check-hardware` (or anything
else that calls `adb kill-server`) while an app deploy/install is in flight.

The skill can restart the adb server as part of its phone probe, which kills any
in-progress `adb install`: an `expo run:android` install failed with a bare exit-1 because
check-hardware was run right after a firmware flash while the install was streaming.
Board-side re-enumeration checks after a flash can use `lsusb | grep 2fe3` +
`fw/scripts/fix-usb-dev-nodes.sh` directly while Metro/expo is mid-deploy. (The probe only
restarts adb when no device is already in `device` state — see
`.devcontainer/scripts/check-hardware.sh`.)

## 2026-08-02 stale cwd made a failed build look green

Rule: `fw/CLAUDE.md` "Build and Test Commands" — run every `west`/`twister`/`pytest` from an
explicit repo/worktree root, and verify freshness before trusting a green re-run.

Observed three times in one session. The agent shell's cwd persists across tool calls:
`west build ... fw` from inside `fw/` printed `ERROR: fw doesn't contain a CMakeLists.txt`
yet the wrapping command could still exit 0; a backgrounded `twister -T fw/tests` from
inside `fw/` died with "No testsuites found" while a **stale** `fw/twister-out/twister.json`
from the previous run read as a plausible all-green result; `pytest tools/tests/` collected
zero tests. `run_in_background` commands capture the cwd at launch, so they are the most
exposed.

## 2026-08-11 PR 325 reset_cause module

Rule: root `CLAUDE.md` "Don't rebuild what already exists" — when the argument for building
something is "the existing one does not work here", check whether YOUR change is what
stopped it working.

A custom `reset_cause` module was written with its own copy of Zephyr's `RESET_*` name
table, justified on the grounds that `CONFIG_HWINFO_SHELL`'s `hwinfo reset_cause show`
"would read 0 and therefore lie". It would only read 0 because that same new module cleared
`RESETREAS` at boot. The justification was a consequence of the thing being justified.
Removing the clear made the built-in work correctly and the custom module unnecessary.

## 2026-08-12 BT stack registration leak on the OnePlus

Rule: root `CLAUDE.md` "NEVER reboot the shared Android phone on your own" — the recovery
ladder.

Four `adb shell svc bluetooth disable`/`enable` cycles left every scan failing with
`SCAN_FAILED_APPLICATION_REGISTRATION_FAILED` (error 6); one
`adb shell am force-stop com.android.bluetooth` fixed it on the next app launch (the stack
restarts itself; `settings get global bluetooth_on` still reads 1 afterwards). Force-stop
the app first either way — its own registrations are part of what leaks. This is strictly
lighter than a phone reboot, so it belongs in the ladder before ever asking the user.

## 2026-08-15 gh api body=@file posted literal paths

Rule: `.claude/skills/submit-pr/references/gh-review-comments.md`.

Five review comments on PR #377 were posted with `gh api ... -f body=@/tmp/c1.md`; `-f` is
verbatim, so each comment's entire body was the literal string `@/tmp/c1.md`. The POST
returned 201 every time, so nothing looked wrong until the comments were read on GitHub.

## 2026-08-24 phone-choice rule relaxed

Rule: root `CLAUDE.md` "Choosing which phone to use".

Superseded 2026-08-24 (maintainer instruction, reconfirmed 2026-08-27): the phone section
previously required the OnePlus 9 Pro (LE2125) specifically and told agents to stop and ask
if it was absent. Any connected Android phone is now allowed — the named phones are the
*known* ones, not the permitted ones. The original rule bundled "use the OnePlus
specifically" together with "do not go connecting things on your own", and only the first
half was lifted.

## 2026-08-26 agent reconnected a dropped ADB session

Rule: root `CLAUDE.md` "NEVER connect an ADB device yourself".

An agent whose phone dropped its wireless-debugging session ran `adb disconnect` and
`adb connect <ip:port>` to try to recover it instead of stopping. The connection is the
user's to make, and an agent probing for it is exactly the autonomy the rule forbids. The
"never connect a device yourself" half of the pre-2026-08-24 phone rule was restored
2026-08-27 (maintainer instruction) after it had been lifted along with the phone-choice
half it was bundled with. The same date withdrew `app/CLAUDE.md`'s old "always try
`adb connect <ip:port>` first" advice.
