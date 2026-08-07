---
name: drive-app
description: Drive the companion app on the physical phone reliably — press the right button, and know when the screen actually changed. Use for any hardware validation that clicks through app UI.
---

# Driving the companion app on hardware

Hold the `app` hardware lock first (root CLAUDE.md "Hardware locking"). Everything below
is `mcp__execbro__*`; the hook denies these without the lock.

This skill exists because of a real, expensive failure (2026-08-07): an agent spent
several minutes of a validation run failing to press an `Install` button, then several
more timing out on waits for a screen that had already changed. Both failures had
non-obvious causes and both are fully avoidable.

## Why the naive approaches fail

**The whole navigation stack stays mounted.** A pushed Expo Router screen does not
unmount its parent (this is also why `firmware-update/` needs a provider-owned MCUmgr
client). So the fiber tree contains every button on every screen below the one you can
see. Measured on the firmware-update flow: `find_components(pattern: "AppButton")`
returned **8** instances; `visibleOnly: true` returned **1**. The visible one was index
**7**.

That breaks two targeting strategies outright:

- **`tap(component="AppButton")` presses the wrong instance and reports success.** It
  resolves a fiber, takes its coordinates, and dispatches a native touch there — which
  lands wherever those (stale, covered-screen) coordinates point, usually on inert text.
  Index 0 was the *landing* screen's "Install Update". The call returned
  `success: true` with a full fiber path, and no handler ran.
- **`index=` is not stable.** Ordering is fiber-traversal order, so it shifts with
  navigation depth and conditional rendering. Never build a wait or a tap on it.

**`tap(text=...)` in the default (fiber) strategy is a case-insensitive substring match
over every `Text` node**, not a label match. `text="Install"` matched three nodes on one
screen: the title "Ready to install", the caption "2 images will be installed.", and the
button. Any step title containing the verb collides with the button that performs it.

**Log polling cannot work.** The `firmware-update/` screens contain **zero**
`console.log` calls outside `debug.tsx` — there is nothing to grep, so a
logcat/`search_logs` wait always times out. During an MCUmgr upload it is worse: the JS
log buffer saturates completely (2000/2000 entries `SMP monitor called!` from
`services/mcumgr.ts`), evicting anything useful within milliseconds. **Never wait on a
log string you have not first confirmed the app actually emits.**

**Coordinates read off a screenshot miss.** Three spaces are in play:

| space | dimensions (Pixel 9 Pro) | used by |
| --- | --- | --- |
| execbro | **896 × 2000** | `tap(x,y)`, `get_screen_state`, `get_screen_layout`, `measure`, `inspect_at_point` |
| raw device px | **960 × 2142** | `adb shell input tap`, `uiautomator dump` bounds, `tap(native: true)` |
| image as rendered in your context | ~703 × 1568 | what you *see* — never a valid tap input |

`device_px = execbro_px × 15/14`. Feeding a `uiautomator` coordinate straight into
execbro overshoots by 7.1%, which resolved to a button on a **covered screen** — wrong
element, wrong screen, silently. Take coordinates verbatim from `get_screen_state`;
never scale, never estimate off the image.

## Decision table

| I want to… | Use |
| --- | --- |
| Press a button that has a `testID` | `tap(testID="fw-update-install")` — always first choice |
| Press a labelled button with no `testID` | `tap(text="Install", strategy="accessibility")` |
| Press an icon-only control | `tap(x, y)` with coordinates **verbatim** from `get_screen_state` |
| Press when several same-named components are mounted | `tap(x, y)` from `get_screen_state`. Not `component=`, not `index=` |
| Find the tap target | `get_screen_state()` — start here after every action |
| Know which fiber instances are real | `find_components(pattern, visibleOnly: true)` |
| Confirm which code renders a pixel | `inspect_at_point(x, y)` — check the ancestor chain names the screen you expect |
| Read a screen's state machine | `inspect_component("Name(./path/to/file.tsx)")` — the `(./path)` suffix is required |
| Wait for a screen change | the recipe below |
| Confirm a tap fired | a state change — see below |

`strategy="accessibility"` is structurally immune to the covered-screen bug: the
accessibility tree only exposes the foreground window. Verified — grepping a
`uiautomator` dump of the flow screen for the five covered landing-screen button labels
found **0 occurrences of each**, while execbro's fiber tree saw all of them. It also
matches per node rather than by substring, so the 3-way "Install" ambiguity does not
arise. `AppButton` sets `accessibilityLabel={title}`, which surfaces as `content-desc`.

## Waiting for a screen change

**Preferred: poll `get_screen_state()`.** Screenshot-free, ~15 lines per call,
`pressablesOnly: true` trims it to ~6. It stayed accurate through an entire BLE upload,
including two moments when `uiautomator dump` returned nothing parseable. **Never put
`android_screenshot` in a polling loop** — it returns a large image and will bury your
context.

Take a **baseline reading before the action**. "It says uploading" proves nothing if you
never confirmed it said "ready" beforehand.

For long unattended waits, run this via `Monitor` or `Bash(run_in_background: true)`
(foreground `sleep` is blocked). The `firmware-update/flow.tsx` `STEP_TITLE` map is 1:1
with the state machine, so the title *is* the state:

```bash
TARGET="Ready to restart"
STEPS='Preparing update|Ready to install|Uploading firmware|Preparing images|Ready to restart|Restarting device|Waiting for device|Verifying installation|Update complete|Update failed'
DEADLINE=$(( $(date +%s) + 300 ))
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  if adb exec-out uiautomator dump /dev/tty 2>/dev/null > /tmp/w.xml; then
    title=$(grep -o 'text="[^"]*"' /tmp/w.xml | sed 's/text="//;s/"$//' \
      | grep -xE "$STEPS" | head -1)
    echo "$(date +%T) step-title=[$title]"
    [ "$title" = "$TARGET" ] && { echo "REACHED"; exit 0; }
    [ "$title" = "Update failed" ] && { echo "TERMINAL-FAIL"; exit 2; }
  fi
  sleep 5
done
echo "TIMEOUT"; exit 1
```

Three load-bearing details: an empty `title` means **unknown**, so the loop continues —
that fired twice under peak BLE load and any "assume unchanged" or "assume failed"
policy would have produced a false result. It exits immediately on the terminal failure
state rather than burning the whole deadline. And it never takes a screenshot.

`uiautomator dump` costs 2.3–2.9 s idle, degrading to 11–16 s with unparseable output
under BLE load. Never trust a single sample.

## Confirming a tap actually worked

Only a **state change** counts:

1. **A hook value moved** — `inspect_component("FirmwareUpdateFlow(./firmware-update/flow.tsx)")`,
   `hooks[N].value` going `"ready"` → `"uploading"`. Locate hooks by *shape*
   (a `FlowStep` string; `{current: <FlowStep>}` is `stepRef`), not by index — indices
   shift when a hook is added.
2. **The step title changed** in `get_screen_state`.
3. **A route change** in `get_screen_state`'s `navigation stack` line, for taps that navigate.

**Never trust `success: true`** — it means "an element was found and touched", not
*which* element. **Never trust `verification.meaningful` or the pixel diff**: a correct
tap on a slow async action changes 0 pixels, and a wrong tap on a covered screen also
changes 0 pixels. The signal cannot separate them.

## Make screens drivable: add `testID`

`AppButton` forwards `testID` already (`Props extends PressableProps`; `{...rest}`
spreads onto the `Pressable`) — that forwarding is load-bearing for this skill, don't
destructure it away. Convention: lower kebab-case, template literals for per-item IDs,
matching existing usage (`shuffle-toggle`, `slot-up-next-${i}`).

When you add a screen that a validation run will click through, give it `testID`s at the
same time. Put one on the **step container** too (`` {`fw-update-step-${step}`} ``) so
state can be read rather than inferred. Caveat: `get_screen_state` prints `testID` only
for pressables — a container `testID` is readable via `inspect_at_point` and in jest,
but not from a `get_screen_state` poll, so the step *title* stays the practical polling
signal.
