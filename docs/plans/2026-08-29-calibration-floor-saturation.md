# Handoff: Audio Tuning stack — ownership transfer

**Status:** active work, handed over 2026-08-29. Uncommitted working note.
**You now own this end to end** — the open design question, the remaining backlog, the hardware
validation, and the six-PR stack. Nothing below is reserved for the previous session.

---

## 0. The shape of the work

A stacked PR chain reimagining the companion app's audio config page as a venue-tunable GUI for
AGC and beat detection:

| PR | Branch | Contents | State |
|---|---|---|---|
| #413 | `audio-tuning-param-table` | Graphical AGC/beat screen + firmware parameter SSOT | 10/10 review threads answered |
| #414 | `audio-tuning-presets` | Presets, A/B compare, undo | 11/11 answered |
| #416 | `audio-telemetry` | Firmware live telemetry service | 15/15 answered |
| #417 | `audio-telemetry-app` | Live telemetry meters in the app | 13/13 answered |
| #418 | `audio-calibration-wizard` | The "Tune it for me" collection board | 16/16 answered |
| #419 | `audio-param-ranges` | Parameter ranges from the device | 8/8 answered |

All six are pushed, CI green, and every review thread has a reply. **None are merged.** They
merge in order; a fix belonging to an earlier PR must land there, not downstream (see §6).

Per-branch gates as of handover — `cd app && npx jest && npx tsc --noEmit && npx expo lint -- --max-warnings 0`:
622 / 911 / 911 / 812 / 911 / 925 tests, tsc and lint clean on each.

**What is hardware-verified:** only the telemetry-arming fix (§4a) and the collection board's
basic operation (§3). Everything else in the stack has passed tests and nothing more. Treat
"tests green" as unverified for anything touching device↔app behaviour — `app/CLAUDE.md`
explains the class of bug the mocked test suite is structurally blind to.

## 1. The open design question (the reason this doc exists)

In a **measurably silent room** the wizard proposes the **maximum** beat floor and justifies it
with a claim about background noise that its own sibling flag rejects.

`app/services/audio-calibration.ts`, `analyzeRoom()` (line 137):

```ts
const band0: number[] = [];
for (let f = 0; f < win.frames; f++) band0.push(win.flux[f * AUDIO_NUM_BANDS]);
const quietFlux99 = percentile(band0, 0.99);          // line 162

const wantedFloor   = 1.2 * quietFlux99;              // line 165
const proposedFloor = clamp(wantedFloor, FLOOR_MIN, FLOOR_MAX);   // line 166

const noisy          = roomP95 > ROOM_NOISY_RMS;      // line 168  -> FALSE  (no warning)
const floorSaturated = wantedFloor > FLOOR_MAX;       // line 169  -> TRUE   (warning below)
```

`ROOM_NOISY_RMS = 0.003` (line 110); `FLOOR_MIN = 0.02`, `FLOOR_MAX = 0.1` (lines 118–119).

`floorSaturated` renders:

> "**The background noise is loud enough** that beats have to be picked out of it. Minimum beat
> strength is already as high as it safely goes — above this it starts eating real beats — so
> raise it by hand only if the lights still twitch between songs."

Both flags come from the same window, so the app concluded "not noisy" and simultaneously said
"the background noise is loud enough".

**The wording is the small half.** `beatFluxFloor` is proposed at 0.100 — its ceiling, which
`FLOOR_MAX`'s own comment says is where it "starts eating real beats" — in a silent room.

## 2. Measured evidence (2026-08-29)

Board: proto0, appcore `75ebf3ea…` (verified via `IMAGE_TLV_SHA256` against `mcumgr image list`,
`active confirmed`, nothing pending), threshold **mode 0** (mean + alpha·sigma), alpha 0.30.
Phone: Pixel 9 Pro / Android 17. Room: domestic, quiet, laptop fan only.

Quiet-room telemetry (`sound telemetry status`, seven samples):

| quantity | observed |
|---|---|
| `rms in-ref` | 0.0002 – 0.0005 |
| `noise floor` | 0.0002 |
| AGC gain | +0 steps throughout, `settled 255 frames` |
| flags | `SILENT` on every sample |
| monitor panel | "Muted 100% of the time" |
| **band-0 flux** | **0.5286, 0.2843, 0.0925, 0.0280, 0.0000, 0.0000, 0.0000** |
| bands 1–3 flux | 0.0000 – 0.0160 |
| band-0 "fires at" | 0.2602, 0.1658, 0.1629, 0.1256, 0.1229, 0.1017, 0.0800 |

Band-0 flux is **spiky in silence** while bands 1–3 stay flat. `percentile(band0, 0.99)` selects
exactly those spikes: `1.2 × ~0.5 ≈ 0.6 ≫ FLOOR_MAX`.

Collection: **67.3 s** background (quiet) + **70.0 s** music, one sitting each. Fit output:

| parameter | current → proposed | shown rationale |
|---|---|---|
| `beatFluxFloor` | 0.080 → **0.100** | "Measured from the quiet room." |
| `agcTargetLow` | 0.0020 → 0.0032 | "Fitted to the quiet parts of the music." |
| `agcTargetHigh` | 0.050 → 0.020 | "Fitted to the loud parts of the music." |
| `agcNoiseGateRms` | 0.00060 → 0.00041 | "Set between the room noise and the quiet music." |

`floorSaturated` fired; `noisy` did not. **Changes were not applied** — the board is still on
defaults, so the run is repeatable.

Derived: the gate proposal 0.00041 = `0.8 × musicP5`, so `musicP5 ≈ 0.00051` — the *music* bound
won, not the room bound. That matters in §3a.

`widened` / `clipAdjusted` (`analyzeMusic`, line 223) did **not** fire: the window came out 6.25×
(above `MIN_TARGET_RATIO = 4`), 35–37 dB headroom, zero clips.

## 3. Ruled out — do not re-propose

### 3a. "Only count frames above the gate" — circular

The gate is a parameter **this same fit adjusts**, and room level feeds it
(`reconcileGate`, line 300):

```ts
const fromRoom  = GATE_ROOM_MARGIN  * roomP95;   // 1.15
const fromMusic = GATE_MUSIC_MARGIN * musicP5;   // 0.80
const noiseGate = clampToSpec("agcNoiseGateRms", Math.max(GATE_MIN, Math.min(fromRoom, fromMusic)));
```

If the filter reaches `roomP95`, every survivor is ≥ the current gate, so `fromRoom ≥ 1.15 × gate`
and **the gate ratchets up every run**, bounded only by `0.8 × musicP5`.

It also hides in test: the ratchet only bites when `fromRoom` binds — a room loud enough to beat
the music bound. Tonight `fromMusic` won, so a quiet-room test shows nothing. It would first
appear at a venue.

Even confined to the flux percentile, it makes `beatFluxFloor` depend on the gate the same fit is
changing — two runs on an unchanged room give two different floors.

**Rule adopted: never condition a measurement on a parameter this fit adjusts.**

### 3b. The firmware `SILENT` flag — identical to 3a

`fw/src/sound/agc_controller.cpp:47`:

```c
d.silent = input_ref < cfg.getNoiseGateRms();
```

`SILENT` *is* the gate comparison — the same circularity wearing a device-side disguise. Worth
keeping from that file (line 23): the comparison is on **input-referred** RMS deliberately, so
`SILENT` is immune to AGC *gain* drift; only the gate parameter contaminates it.

## 4. The options

### A — an absolute RMS constant
Simple, but invents a second unmeasured magic number while we are trying to retire the first
(§5b).

### B — the AGC's own noise floor
`fw/src/sound/agc_controller.cpp:39-44`:

```c
if (...) noise_floor_ = input_ref;                            // initialise
else if (input_ref < noise_floor_) noise_floor_ = input_ref;  // instant drop
else noise_floor_ += (input_ref - noise_floor_) * 1.0e-4f;    // slow rise
```

A **minimum tracker** on input-referred RMS: fast attack, very slow release, **no dependence on
`agcNoiseGateRms`**. Already on the telemetry wire. Genuinely gate-independent — strongest on
that axis. Open: it adapts, so it is a moving reference.

### C — the shape of the flux distribution
Band-0 p99 ≈ 0.5 on a median ≈ 0. A real noise bed produces *sustained* flux; a silent room
produces isolated transients. So `p99 ≫ p75` is itself the signature, using **no external
parameter** — non-circular by construction. Unvalidated: if a genuinely noisy room also shows
`p99 ≫ p75`, it dies.

### D — fix it upstream in the DSP
Log-flux is a ratio; near the quantisation floor a tiny absolute change is a large log change.
The firmware could refuse to emit flux below an absolute energy. Most principled, riskiest: it
changes beat detection itself, and `audio_analysis_result`'s layout is frozen for
msgq/tap/extension-ABI compatibility (`fw/src/sound/audio_dsp.h:92-100`).

### Whatever is chosen
When the criterion says the room produced nothing measurable, the likely right behaviour is
**leave `beatFluxFloor` alone**, propose no change, and say so — no proposal, no warning, nothing
to contradict. Band-0 selection stays; it is deliberate (`audio-calibration.ts:156-158`: crowd
noise is broadband and sustained, a kick is impulsive and band-limited, so band-0 separates them
long after RMS cannot).

### The decisive missing measurement
**Every option exists to separate "quiet room with transients" from "genuinely noisy room", and
only the first has been measured.** A crowd-noise background sample decides it: A needs a real
noisy room's `roomP95`; B needs room RMS vs tracked floor in both conditions; C dies immediately
if a loud room also shows `p99 ≫ p75`.

Procedure — **collectors accumulate** (`drain()` → `appendChunk`, so sittings *merge*): discard
the existing background pool ("Discard the last one") **before** collecting crowd, or the two
average into a measurement of neither. Then ~60 s of crowd noise, music off, sampling
`sound telemetry status` throughout.

## 5. The rest of the backlog

### 5a. `widened` / `clipAdjusted` are computed, tested, and never shown
`analyzeMusic` (line 223) silently corrects its own proposal twice: `clipAdjusted` pulls the
ceiling down 25% when the mic was overdriven; `widened` drags an end of the window to satisfy
`MIN_TARGET_RATIO = 4`. Both flags are returned and asserted in tests, and the hook drops them.
The neighbouring steps both have a channel for exactly this (room has `warnings[]`, taps has
`notes[]`), which is what makes it an oversight rather than a decision. Needs deliberately
clipping or heavily-compressed music to observe.

### 5b. `ROOM_NOISY_RMS = 0.003` has no measured basis
Its own comment records that the previous bound was the level at which the wizard's **own** gate
clamp began to saturate — a limit it drew for itself. It now only sets the `noisy` flag, so a
wrong value mis-warns rather than blocking. The same crowd sample gives it a real basis.

### 5c. Tempo double/half — a product call, not a defect
`classifyTempo` detects both and the hook surfaces both as notes. The asymmetry: the `double`
case computes an exact `proposedRefractoryFrames` and then tells the user to type it in by hand
("try a gap of about N ms in Advanced"); the `half` case has no proposal at all. Question for the
maintainer: should either become a one-tap fix?

### 5d. `friendlyLabel` collision — recommend closing
`beatAlpha` and `SENSITIVITY_MACRO_SPEC` both read "Sensitivity", but nothing renders both:
Advanced hides whichever threshold parameter the device is not using, and the Simple row already
overrides `key` and `firmwareLabel` per mode (`audio.tsx:454-461`). Verified non-issue.

### 5e. Full-stack validation — never done
Crowd noise + music together, the venue scenario the rework exists for. The collection board's
premise is that it works in a room that is loud all the time with music starting and stopping on
the band's schedule. Untested.

### 5f. Filed firmware issues (deferred, not blocking the stack)
- **#423** — export `band_threshold[]` from `audio_analysis_result` instead of re-deriving the
  fire line in telemetry. Three copies of the formula exist; a new threshold mode silently leaves
  the meter drawing the old one.
- **#424** — replace the mirrored DFU/stream conn-param holds with a per-source table. Six
  mirrored sites; the `downgrade_timer_ms()` one fails invisibly if a seventh is forgotten.

## 6. What landed this session

**Telemetry arming is now demand-driven (#418, commit `1c71298`).** Found on hardware: with the
phone on the Controls animation list — a screen with no meter — the board logged
`telemetry stream started: tier 3, 8 Hz, hold 60 s` and `conn param request: MEDIUM`, dragging
the interval 11.25 ms → 45 ms and re-arming every 30 s so the governor could never reach its idle
downgrade.

Cause: #418 hoists `AudioTelemetryProvider` to the device-state **stack layout** (correct, for one
provider), but arming was gated on that provider being *focused*, and a stack layout is focused
whenever any screen in it is. Fix: `useAudioTelemetryStream()`, armed while ≥1 consumer is held,
released on blur with a 750 ms grace (expo-router blurs the outgoing screen before focusing the
incoming one).

Verified on hardware, all three transitions:

```
Controls      conn param request: SLOW (idle downgrade), 157.50 ms | Telemetry: idle
Audio Tuning  telemetry stream started: tier 3, 8 Hz | MEDIUM, 45 ms | streaming
back          telemetry stream stopped (app request)  | Telemetry: idle
```

The SLOW downgrade is new — the re-armed hold previously made it unreachable.

**Attribution matters here:** the previous session first told the maintainer this was a #417 bug.
It is not — on #417 the provider is mounted inside `audio.tsx`, so the stream only arms with that
screen up. The regression is introduced by #418's hoist. Correcting that changed which branch the
fix belonged to.

**Also this session:** the collection board ran on hardware for the first time — collectors,
pools, "✓ 67.3s collected · 1 go", discard-last, the fit gate ("Needs background noise and music
before it can work anything out"), and the review screen all behaved. The **apply** path was
deliberately not exercised, and tap-along was never driven with real taps.

## 7. Hardware

Bench at handover: proto0 board detected (both CDC ports), **no J-Link** — OTA only, ~4 min per
flash via `fw/scripts/mcumgr-flash.sh`. Pixel 9 Pro on WiFi ADB.

**Locks are released as of this handover.** Take them before touching anything:

```
Monitor(command: "scripts/hw-lock.sh hold board", persistent: true)
Monitor(command: "scripts/hw-lock.sh hold app",   persistent: true)
timeout 15 bash -c 'until scripts/hw-lock.sh check board >/dev/null 2>&1; do sleep 0.5; done'
```

Reproduce:

```bash
.devcontainer/scripts/check-hardware.sh          # never a bare `adb devices` / `lsusb`
west build --build-dir fw/build fw --board rgb_sunglasses_proto0/nrf5340/cpuapp --sysbuild -- -DBOARD_ROOT="$(pwd)/fw"
fw/scripts/mcumgr-flash.sh
python3 fw/tools/dump_dfu_tlv.py fw/build/dfu_application.zip   # compare vs `mcumgr image list`
app/scripts/launch-app.sh                        # background task; never `npx expo run:android`
```

`sound telemetry status` on the Zephyr shell prints the frame the app receives: rms in-ref, noise
floor, flags, per-band flux, resolved "fires at" threshold. It is the fastest instrument here.

## 8. Traps that cost time this session

- **Flush serial before every read.** Extension shuffle floods the shell with llext relocation
  logs; a single read pulled 20 KB of noise and missed the answer. `log` is **not** a registered
  shell command on this build, so the module cannot be silenced — turn shuffle off in the app
  instead.
- **`pgrep -f "west build …"` matches your own waiter shell.** A wait loop built on it never
  exits. The build had finished 14 minutes earlier.
- **Never background a build with `nohup … &`.** The harness sees the wrapper exit and reports
  success while the build is at step 124/140. Use `run_in_background: true` with the bare command.
- **The calibration screen reflows after every interaction.** Re-read `get_screen_state`
  immediately before each tap; a stale coordinate started the Tap-along collector instead of
  Music. Fiber taps also mis-landed here — coordinate taps from a fresh screen state are reliable
  on the Pixel.
- **`-80 dBFS` on the monitor panel is the axis label, not a reading.** Misreported once
  historically.
- **Apply the fix to the branch that introduced it** (§6). Check whether the file even exists on
  the earlier branch before assuming attribution.

## 9. Standards this maintainer holds work to

- **Mutation-check every behavioural claim**: reintroduce the original bug, confirm a test fails.
  Tests here have passed vacuously more than once — `act()` flushes effects on exit, so advancing
  timers inside the same `act()` as a rerender runs them before the effect that schedules them.
- **Never propose reducing test or CI coverage** as a remedy for infrastructure pain. This was
  stated explicitly and firmly.
- **Never connect or switch an ADB device autonomously.** If nothing is connected, say so and
  stop. Any connected phone may be used; the named handsets are the *known* ones, not the only
  permitted ones.
- Hardware claims are quoted from device output, never inferred.
- Read `fw/CLAUDE.md` before touching firmware and `app/CLAUDE.md` before touching the app; the
  root `CLAUDE.md` holds the task-routing table and the hardware-lock discipline.

## 10. What needs the maintainer, and what is yours

**Yours to decide:** which of A–D in §4; how to surface `widened`/`clipAdjusted`; the wording of
any replacement copy; how to structure the fix across the stack.

**Needs the maintainer:** anything that changes shipped preset values or default behaviour; the
tempo double/half product call (§5c); whether to merge the stack before or after these fixes;
and any decision to reduce scope.

---

## 11. RESOLVED — crowd-noise sweep and the fix (added 2026-08-29, second session)

The §4 "decisive missing measurement" was taken as a **volume sweep**, not a single sample:
crowd noise played from the maintainer's phone at three levels plus a silent control, ~100 s
each, same room, same night, board on defaults, collected through the wizard's own collection
board (pools extracted via the app, sittings discarded between levels so nothing merged).
The single-level version of this measurement was rejected first: the maintainer had calibrated
the phone's crowd volume against the music source for a "venue-plausible" SNR and flagged it
as suspect — correctly. Any absolute RMS constant derived from one blessed level is the
assumed SNR read back through a microphone. The sweep removes the need to bless a level.

| condition | rms p5 / p50 / p95 | band-0 flux p75 / p95 / p99 | exact-zero frames | wantedFloor (1.2×p99) | `noisy` |
|---|---|---|---|---|---|
| silent | 0.0002 / 0.0002 / 0.0006 | 0.14 / 0.53 / 1.19 | 49% | 1.43 — saturated | false |
| crowd, barely audible | 0.0003 / 0.0004 / 0.0008 | 0.21 / 0.84 / 1.42 | 51% | 1.71 — saturated | false |
| crowd, half volume | 0.0014 / 0.0021 / 0.0034 | 0.38 / 1.33 / 2.08 | 50% | 2.50 — saturated | true |
| crowd, venue-loud | 0.0035 / 0.0050 / 0.0067 | 0.21 / 0.53 / 0.83 | 52% | 1.00 — saturated | true |

Raw arrays: scratchpad `crowd-sweep/*.json` (session-local; the table above is the durable
record). Serial `sound telemetry status` cross-checks agreed throughout; during venue-loud
crowd the detector fired beats on defaults (`beats 0x5`), and during barely-audible crowd the
firmware flagged SILENT.

**What the sweep settled:**

- **Option C is falsified.** Real crowd noise produces the same band-0 flux distribution
  shape as silence: ~50% exact zeros in every condition (structural — flux is rectified and
  energy falls half the time), p99 ≫ p75 everywhere, magnitude non-monotonic in level (half
  volume produced the night's largest flux). No flux-domain statistic separates a real bed
  from quantisation junk.
- **Option B's moving reference was observed live**: the AGC noise-floor tracker crept
  0.0002 → 0.0014 within minutes of sustained noise and snapped back instantly when the level
  fell. Over a venue evening it compresses toward the bed; the reference for "is there
  anything real here" should be the hardware floor, which does not move.
- **The RMS domain separates monotonically, 11× between silence and venue-loud.** The honest
  absolute anchor is the mic's own self-noise (p95 ≈ 0.0006–0.0008 input-referred) — a
  hardware property, identical at any venue. "Barely audible to a human" lands inside it.
  `ROOM_NOISY_RMS = 0.003` sits 4–5× above the hardware floor; the sweep brackets it from
  both sides. §5b is addressed to the extent a bench can: the constant now has a measured
  self-noise anchor, not a venue basis (only real venue telemetry could give it that).

**The fix (chosen: level-gated fit — option A's domain with no new constant):** `analyzeRoom`
computes `noisy` first; when the room is not noisy the flux percentile is log-noise by
construction, so `proposedFloor` is **null** — no proposal, no saturation warning, nothing for
the `noisy` flag to contradict — and the hook notes "The room was quiet, so Minimum beat
strength has been left alone." When the room IS noisy the reviewed behaviour is unchanged
(fit 1.2×p99, cap at FLOOR_MAX, warn), and the saturation copy's claim about background noise
is now true whenever it appears. The sweep replay uses the device's current floor when the
fit proposes none. §3's rule holds: `ROOM_NOISY_RMS` is a fixed constant, not a parameter the
fit adjusts, so nothing ratchets. Mutation-checked: the gate removed, the hook's null-check
removed, and the sweep fallback constant-folded each fail tests.

**Flagged to the maintainer, not acted on:** `wantedFloor` saturated in every condition,
including genuinely loud ones (1.0–2.5 against a cap of 0.1). The floor fit may never produce
an in-range answer for any real room, which would make the noisy-room proposal row vestigial
(it can only ever say "cap"). Keep / cap-and-warn (current) / drop is a product call.

## 12. Session close-out (2026-08-29, second session)

- **§5e executed** — the full venue scenario on hardware, first time: 269 s crowd background
  (level raised mid-sitting; loud tail read rms 0.0053 at the board) + 113 s music-over-crowd
  (rms 0.0114). Fit produced the coherent loud-room proposal: floor 0.080→0.100 (cap, with
  the saturation warning now true), targets 0.002→0.005 / 0.05→0.02, gate 0.0006→0.0057
  (music bound won), all three warnings consistent. **The apply path ran for the first time**:
  4/4 writes verified against `sound dsp params` / `sound agc status` readbacks, the
  "Before calibration" preset was saved first, and applying that preset restored every value
  (verified by the same readbacks). Board left on defaults.
- **§5a shipped** — `clipAdjusted` surfaces as a warning, `widened` as a note (commit
  2402a05); the venue run demonstrated the gap live (window widened to exactly 4x while the
  review said "fitted to the loud parts of the music").
- **Still with the maintainer**: §5c (tempo double/half one-tap fix?), the vestigial-floor-row
  question (§11), and when to merge the stack.

## 13. Tap-along hardware pass (2026-08-29, late)

Real fingers drove the tap pad for the first time. First attempt looked like a product bug —
pad showed press feedback, count frozen at 0, then every button went touch-dead — but stage
counters (temporary instrumentation, since reverted) on a clean relaunch proved the code
sound: 80/80 taps flowed pressIn → press → recordTap with zero guard failures, 39 s of
32 Hz undecimated tap window, and the fit produced the tap-derived rows on hardware for the
first time (beatAlpha 0.30→0.193 "Matched 45% of your taps, up from 32%", refractory 5→12).
The touch-dead state was environmental: agent-driven `navigate()` calls had stacked duplicate
audio-calibrate instances (dev-mode nav restore resurrected them after reload), which wedged
the app's entire touch pipeline — not reachable by real-user navigation, not present in
production builds (no dev nav restore). Lessons recorded in app/CLAUDE.md.

Two hardening changes landed from the investigation: `cancel()` now clears `state.active`
alongside the ref (a stale pad would otherwise swallow taps silently — press feedback with a
frozen count), and TapPad gained wiring-level Pressable tests, since the hook suite calls
recordTap() directly and is structurally blind to the component the finger touches.
