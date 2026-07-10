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
   read `app/CLAUDE.md` first for launch/tap procedures — launch only via
   `app/scripts/launch-app.sh`, never `npx expo run:android` directly) and confirm
   discovery completes with no fallback/mismatch warnings.
2. Exercise every changed read/write/notify path end-to-end **and cross-check against
   the firmware's own source of truth** (the `mcp__serial__*` shell, e.g. `ext param`,
   `glim`, `anim get`, `bt_conn_info`), not just the app UI — optimistic updates make the
   UI lie (see app/CLAUDE.md "Verifying a write/notify round-trip").
3. If the change involves notifications, verify the app *receives* them (a value
   changes in the app without a re-read) — notify failures are firmware-log-only and
   completely silent app-side.

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
