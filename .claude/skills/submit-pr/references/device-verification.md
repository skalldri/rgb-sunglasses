# On-device + companion-app verification procedure

Referenced from `/submit-pr` step 5. Do this only after deciding the branch could
affect device↔app communication (trigger list in SKILL.md).

## 1. Hold BOTH hardware locks up front, together

`hold` is the only way to take a lock, launched via `Monitor` (full lock discipline:
root CLAUDE.md "Hardware locking"):

```
Monitor(command: "scripts/hw-lock.sh hold board app", description: "board+app hw-lock heartbeat for submit-pr verification", persistent: true)
```
```bash
timeout 15 bash -c 'until scripts/hw-lock.sh check board >/dev/null 2>&1 && scripts/hw-lock.sh check app >/dev/null 2>&1; do sleep 0.5; done'
```

If this fails, report who holds the conflicting resource(s) and stop — never flash or
drive the phone without the locks. (Distinct from "no board or phone present at all" —
in that case skip locking and go straight to the `AskUserQuestion` waiver in SKILL.md.)

## 2. Flash and verify

Flash with `fw/scripts/jlink-flash.sh` (it self-refuses without the `board` lock), then:

1. Connect the app (phone via ADB + execbro, or ask the user to drive their phone;
   launch via `/launch-app` — never `npx expo run:android` directly — and drive it
   via `/drive-app`) and confirm
   discovery completes with no fallback/mismatch warnings.
2. Exercise every changed read/write/notify path end-to-end **and cross-check against
   the firmware's own source of truth** (the `mcp__serial__*` shell, e.g. `ext param`,
   `glim`, `anim get`, `bt_conn_info`), not just the app UI — optimistic updates make the
   UI lie (see "Verifying a write/notify round-trip" below).
3. If the change involves notifications, verify the app *receives* them (a value
   changes in the app without a re-read) — notify failures are firmware-log-only and
   completely silent app-side.

## Verifying a write/notify round-trip — don't trust a single "it updated" observation

A characteristic whose write-value and notified/stored value differ (e.g. any dropdown-list
characteristic, see `app/components/characteristic-dropdown.tsx`) is easy to mis-verify, because
several distinct bugs all produce the _same_ surface symptom: "I picked an option and the UI
showed the new value." That observation alone does not distinguish:

- a correct write + correct notify (the real success case),
- an optimistic update that clobbers the real value before the (possibly failed) notify arrives,
- a no-op: the option tapped happened to match what the UI already (possibly stale) believed was
  selected, so no write was even sent,
- a notify that silently failed (e.g. exceeded the negotiated MTU —
  `.claude/rules/ble-gatt-contract.md`) while the UI happened to already show the right value
  from a stale read.

What actually caught the MTU/notify bugs in this codebase: reopening the picker afterward to
confirm _all_ options are still listed (not just the one that appeared selected), and
cross-checking the characteristic's value against the firmware's own source of truth immediately
after the write (the `glim` shell command, via the `mcp__serial__*` tools) — not a
different/unrelated characteristic. When verifying any BLE write, always do both before calling
it confirmed.

## 3. Always release both locks when finished

Whether verification passed, failed, or was waived after acquiring — stop the `hold`
Monitor task (`TaskStop`; its exit trap releases automatically) or run:

```bash
scripts/hw-lock.sh release board app --force
```

## Why this gate exists

Shell-level testing cannot see BLE-visible state. On PR #89 the extensions' Is Active
mirror was completely dead (a registration-ordering bug returned `-ENOENT` into an
ignored return value) while every build, Twister suite, and serial-shell check passed —
only a real app connection exposed it, plus a second bug (a pushback notification
losing a race against the app's optimistic update) that needed an ATT error instead.
