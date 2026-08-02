# Beat detection & AGC (issue #264): Phase 3+ handoff plan

**Status date: 2026-08-02.** This document is a complete handoff for continuing
issue #264. It assumes the reader has NO prior session context. Read this fully
before writing code, alongside `fw/CLAUDE.md` (workflow traps) and
`fw/docs/beat-detection-debugging.md` (the measurement rig's user guide).

---

## 1. Where things stand

### Shipped (merged to main)

- **PR #275 — the debugging/measurement environment.** Audio tap +
  `sound mic record_wav` (WAV + per-frame analysis sidecar CSV, sector-aligned
  batched writes, QSPI-error retry, direct-capture fallback when the DSP thread
  is dead), `sound dump`, `sound agc freeze/gain`, `sound dsp params/set`, the
  native_sim WAV-replay harness (`fw/tests/sound/audio_dsp_replay/` — compiles
  the REAL `audio_dsp.cpp`), and `fw/tools/beat_lab/` (frames codec, replay
  wrapper with `--sweep`, device-vs-host `compare` gate, mir_eval `evaluate`,
  matplotlib `report`). All firmware pieces gated by `CONFIG_APP_AUDIO_DEBUG`
  (default y, ~33 KB RAM, reclaimable).
- **PR #277 — Phase 1: gain-step compensation.** AGC gain steps no longer wipe
  the detector's 1 s adaptive-threshold history;
  `audio_dsp_compensate_gain_change(steps)` scales the linear-domain
  previous-frame energy instead (exact at all levels; `|steps| > 4` falls back
  to full reset). CRITICAL ordering contract (found by review, verified on
  hardware): compensation must be applied AFTER `audio_dsp_process()` consumes
  the last old-gain block — see the contract comment on the function and the
  `test_gain_compensation_misordered_is_harmful` ztest. On-device result:
  threshold-history retention across a gain step went from 3% to 98–102%.

### Open

- **PR #279 — Phase 2: AgcController** (branch `beat-phase2-agc`, rebased onto
  main, MERGEABLE, review round 1 fully addressed). Extracts AGC decision logic
  into BT-free/Zephyr-free `fw/src/sound/agc_controller.{h,cpp}` compiled
  identically into firmware + unit suite (`fw/tests/sound/agc`) + replay
  harness. Policy: near-clip fast path (peak ≥ 32000 → −2 steps, no rate
  limit), attack (3 consecutive instantaneous-RMS frames > targetHigh → −1),
  release (15 consecutive smoothed frames < targetLow → +1, never while
  silent), **input-referred noise gate** (smoothed RMS normalized to 0 dB park
  gain; silence → hold gain, suppress all `beat[]` output, park to 0 dB after
  ~10 s). Targets derived from captures: targetLow 0.002, targetHigh 0.05,
  gate 0.001. Hardware-verified 4/4 tests (see §4 numbers).
  - If #279 has merged by the time you read this: branch Phase 3 off `main`.
  - If still open: stack Phase 3 on `beat-phase2-agc` and expect the same
    post-squash-merge rebase dance (`git rebase --onto origin/main
    <old-base-ref> <branch>`; it has been needed after every squash merge).

### The problem, one paragraph

Beat detection responds to audio but not in time with the music. Root causes
found and fixed so far: (Phases 1–2) the AGC destroyed detector state on every
gain step and amplified quiet-room noise into false beats. Remaining (Phase 3+):
the **threshold shape** — `mean + α·σ` over a flux history that contains the
beats themselves inflates σ and mutes the detector on steady music — and
**timing jitter** (±160 ms spread even when the fire *rate* is correct),
which needs finer hop resolution and eventually beat-grid/tempo tracking.

---

## 2. The detector today (code map)

All in `fw/src/sound/`:

| File | Role |
|---|---|
| `audio_dsp.{h,cpp}` | 512-pt Hann FFT @16 kHz (32 ms frames, no overlap) → power spectrum → 4 bands (bass bins 1–6, low-mid 7–25, mid 26–63, high 64–191; mean power) → per-band log-spectral flux `max(0, log1p(γE) − log1p(γE_prev))` → 32-frame flux history ring → threshold `mean + α·σ` (arm_mean/arm_std) → `beat[b]` with floor + 5-frame refractory. Also 20 display buckets. Gain-ratio helpers (`audio_dsp_gain_amplitude_ratio/power_ratio`, header-inline) are the ONLY encoding of 0.5 dB/register-step. |
| `agc_controller.{h,cpp}` | Phase 2 AGC policy (see above). Zephyr-free on purpose — compiled into unit tests and the replay harness. |
| `sound.cpp` | PDM capture thread (`audio_dsp_thread_func`), applies AgcController decisions via `agc_apply_gain()` (mutex-serialized, int-clamped, compensates detector + rescales controller RMS window), noise-gates `result.beat[]` (skipped while frozen), audio tap, all `sound *` shell commands, PDM self-heal. **Ordering invariant: process the block BEFORE applying a gain decision** (spelled out in comments; do not re-order). |
| `audio_config.{h,cpp}` | BT/settings-backed provider (12 persistent characteristics under `audio/`). NOTE `getTargetHigh` clamps to **[0.02, 0.5]** as deliberate settings *migration* — the raised floor corrects stale Phase-1 values whose semantics changed. Follow this pattern if Phase 3 changes a key's meaning. |
| `audio_tap_format.h` | Shared D-line/#PARAMS text format (firmware + replay + `frames.py` decoder must stay in sync — this header is the single source of truth for field order). |

Runtime tunables (BLE `audio/` keys + `sound dsp` / `sound agc` shell):
`flux_gamma` 1000, `beat_flux_floor` 0.005 (provably inert — see §4),
`beat_alpha` 3.5 (device currently has **1.5 persisted** — see the caveat in
§6), `beat_refractory_frames` 5, `agc_target_low` 0.002, `agc_target_high`
0.05, `agc_rate_limit_frames` 10, `agc_attack_frames` 3,
`agc_release_frames` 15, `noise_gate_rms` 0.001, plus 2 FFT-display values.

---

## 3. The measurement rig (how every change gets validated)

The whole point of the project so far: **no tuning by vibes.** The loop:

1. **Record on-device** (needs the user to play music; any EDM works):
   `sound agc gain 0x30` (freeze at a known gain) → `sound mic record_wav 30
   /NAND:/clip.wav` → produces WAV + `.csv` sidecar with per-frame analysis.
   Unfrozen recordings capture live AGC behavior instead (gain column in the
   CSV). 180 s cap; free-space guard tells you the real ceiling.
2. **Pull via USB MSC** (mount `/dev/sdX` ro; identify by `RGB-SG` SCSI string;
   see `fw/CLAUDE.md`). If you *deleted* files (rw mount) you MUST reboot the
   board afterward (FAT cache).
3. **Replay on host**: `python3 fw/tools/beat_lab/replay.py --wav clip.wav
   --params-from clip.wav.csv --out host.txt` — `--params-from` is MANDATORY
   for device comparisons (persisted device values differ from compiled
   defaults; omitting it produced 175 phantom mismatches once).
4. **Validate the replica**: `python3 -m tools.beat_lab.compare --device
   clip.wav.csv --host host.txt` (run from `fw/`). Must print PASS. It excludes
   32 warm-up frames and post-gap frames, warns on #PARAMS mismatches, and
   uses atol+rtol float tolerance. 100% beat-mask agreement is the norm.
5. **Score**: `python3 -m tools.beat_lab.evaluate --frames host.txt --wav
   clip.wav --ref-librosa beats --json` (mir_eval F-measure, ±50 ms window).
6. **Sweep** (no rebuild — params are env-driven): `replay.py --wav clip.wav
   --sweep "alpha=1.0:3.5:0.5,floor=0.005:0.05:0.005" --ref-librosa beats
   --band 0`.
7. **Visualize**: `python3 -m tools.beat_lab.report --frames ... --wav ...
   --ref-librosa beats --out report.png` (spectrogram + per-band flux vs
   threshold + beats + RMS + gain track).

The AGC closed loop is also simulated offline: `--agc sim` runs the REAL
`AgcController`; `sim_legacy`/`sim_reset` preserve the Phase-1/pre-Phase-1
behaviors for A/B chains. All sim modes share the gain trajectory so A/Bs
isolate a single variable.

### CRITICAL: the capture corpus is ephemeral

All captures so far lived in the session scratchpad, WHICH IS GONE. The clips
are NOT precious — the rig regenerates everything in minutes with any music.
**First hardware session of Phase 3: record a fresh corpus** (recommended: one
60 s frozen-gain EDM clip at moderate volume for the primary baseline, one 40 s
quiet-room unfrozen, one 30 s at a different volume/genre). Consider committing
a canonical clip set to a release artifact or a `fw/testdata/` LFS-style
location if repeat comparability across sessions matters (decide with the
user; a 60 s WAV is ~1.9 MB).

---

## 4. Measured facts (the numbers everything is calibrated against)

From the ABGT 250 baseline (60 s, frozen +9 dB, ~123 BPM) unless noted:

| Fact | Value |
|---|---|
| Stock detector (α=3.5) F-score, ±50 ms vs librosa beats | **0.023** (band 0 fired 7×/60 s — σ-inflation mutes it) |
| α=1.5: band-0 fire count | **exactly 123 = the librosa reference count**; F=0.211 (rate right, timing jittery) |
| Detection-offset distribution at α=1.5 | median 0 ms, σ≈222 ms, only 26/123 within ±50 ms; vs onsets P=0.50 |
| `beat_flux_floor` sweep 0.01–0.09 | **zero effect** on any metric (floor is inert at 0.005–0.09) |
| Phase-1 A/B (α=1.5, trajectory-matched) | compensation: 123 fires / union F 0.207; legacy reset: 142 fires (19 junk) / 0.186 |
| Music smoothed RMS at +9 dB | min 0.0021 / p50 0.0052; instantaneous p99 0.0137; peak 3.2% FS |
| Room noise (input-referred, 0 dB) | ~0.0006 (varies by night: 0.00057–0.0016 at +9 dB across two sessions — this variance broke the first output-domain gate design) |
| Phase 2 on-device | quiet room: gain holds 0 dB, park at exactly 312 frames, 0 beats/40 s (was +20 dB pinned, 711 beats/90 s); music: 3 settle steps/45 s then 0 (was 16–24/30 s) |
| Second baseline clip (`newbase`, 30 s frozen +4 dB, α=1.5) | 78 librosa beats (~156 BPM), band0 F=0.299 |
| Compare-gate fidelity | 100% beat-mask agreement, fields to ~5e-9 (energy) — the harness is trustworthy |

Roadmap + evidence is also on the issue:
<https://github.com/skalldri/rgb-sunglasses/issues/264> (comment with the
5-phase plan).

---

## 5. PHASE 3 — threshold shape (the next PR)

**Goal**: replace/augment `mean + α·σ` with the doc-specified running
**median + delta**, retune the inert floor, and finalize defaults with sweep
evidence. Expected size ~120 LOC + 2 characteristics + tests + sweeps.

### 5.1 Design

1. **`audio_dsp.cpp`**: add threshold mode switch.
   - New provider getters/setters on `AudioDspConfigProvider` (mirror the
     existing get/set pattern exactly, including clamps in ALL THREE
     implementations — `DefaultAudioDspConfigProvider` in audio_dsp.cpp,
     `AudioConfig` in audio_config.cpp, `EnvConfigProvider` in the replay app;
     the review WILL flag a missing one):
     - `getSfDelta()` — default 0.10 (sweep decides final), clamp [0.0, 2.0]
     - `getThresholdMode()` — 0 = legacy mean+ασ, 1 = median+delta; clamp [0,1]
   - In the per-band loop (step 4d): mode 1 computes
     `threshold = median(flux_history[b]) + sfDelta`. Median via a copy of the
     32-entry ring + `std::nth_element` (or insertion sort — <5 µs either way;
     this runs 4×/frame on a 128 MHz M33F, do NOT over-engineer). Fire
     condition otherwise unchanged (refractory + floor still apply).
   - `out->band_mean/band_sigma` carry median/threshold in mode 1 (document in
     `audio_dsp.h`; struct layout unchanged so the msgq, tap format, extension
     ABI, and all consumers are untouched). NOTE `report.py` reconstructs the
     threshold as `mean + α·σ` — teach it mode-awareness (read
     `threshold_mode` from #PARAMS; in mode 1 plot `band_sigma` directly as
     the threshold).
2. **New persistent characteristics** in `audio_config.cpp`: `audio/sf_delta`
   (float 0.10) and `audio/threshold_mode` (uint32 0 initially — flip the
   default to 1 ONLY after the corpus sweep shows mode 1 ≥ tuned mode 0).
   APPEND after the existing providers in the `BtGattServer(...)` list —
   UUIDs are positional; appending preserves existing ones. Check whether the
   CCC store and the metadata blob sizes still fit (adding 2 more notify
   characteristics; CCC max is already at the 96 ceiling — if the count
   overflows again the notify flag on these two can be false, they're tuning
   knobs not telemetry).
3. **Shell**: extend `sound dsp set` with `sf_delta` and `mode`; `sound dsp
   params` prints them.
4. **`#PARAMS` wire format**: append `sf_delta=<hex8> mode=<u>` via
   `audio_tap_format.h` (single source of truth — update firmware wrapper,
   replay emitter, `frames.py` float-key list, `compare.py` guard list,
   `replay.py --params-from` copy list, and the pytest fixture. ALL SIX. The
   #279 review caught exactly this class of omission).
5. **Replay env plumbing**: `BEAT_SF_DELTA`, `BEAT_THRESHOLD_MODE` +
   `--sf-delta`/`--threshold-mode` flags + sweep support (`--sweep
   "sf_delta=0.02:0.3:0.02"`).
6. **Floor retune**: sweep `beat_flux_floor` in mode 1 on quiet-room capture
   (its real job: absolute lower bound against noise-flux when the median is
   ~0). Decided previously: NOT gain-scaled (log-flux is gain-invariant in the
   loud regime; the Phase 2 gate owns quiet rooms).
7. **Band 0 starts at bin 2** (one-line change to `band_bin_start[]`): bin 1
   (31 Hz) is below the PDM mic's response — the display-bucket table already
   skips it with a comment. Update the band comment block. This shifts band-0
   energies slightly; re-run sweeps AFTER this change, not before.
8. **Defaults finalization**: with the fresh corpus, sweep α (mode 0) and
   sf_delta (mode 1) per clip. Decision rule agreed with the user: mode 1
   becomes default iff its best F ≥ mode 0's best F on EVERY corpus clip AND
   its F-vs-delta curve is visibly flatter (the robustness claim — one delta
   across venues — is the whole justification). Also decide the shipped alpha:
   the measured rate-correct value is 1.5, the compiled default is still 3.5.
   Whatever ships, remember **persisted values override compiled defaults on
   provisioned boards** — the shared board already has α=1.5, targets
   0.002/0.05 persisted. If mode-1 semantics make a persisted value invalid,
   use the clamp-floor migration pattern from `AudioConfig::getTargetHigh`.

### 5.2 Tests (ztests in `fw/tests/sound/audio_dsp/`)

- **Beat-train robustness** (THE motivating case): silence warm-up, then a
  loud burst every 16 frames (~117 BPM at 32 ms frames) for ≥3 history
  lengths. Assert mode 1 keeps firing on every burst after the history fills;
  assert mode 0 at α=3.5 stops firing (documents the σ-inflation failure —
  analogous to how `test_gain_compensation_misordered_is_harmful` pins a
  hazard).
- Mode switch respected at runtime (provider override mid-stream).
- Median math sanity (known history → known median; even-length ring — pick
  and document lower-middle vs mean-of-middles, match what `nth_element`
  gives you).
- Silence stays silent in mode 1 (floor does its job when median≈0).
- `sf_delta`/`threshold_mode` clamp round-trips (extend
  `test_default_provider_setters_clamp`).

### 5.3 Acceptance (before the PR)

- Offline: corpus table (per clip × mode × best-tuned param) showing mode-1 F
  ≥ mode-0 F, plus the delta-sensitivity curve. Put the table in the PR body.
- Device: flash, `sound dsp set mode 1`, A/B live with music (the runtime
  switch exists precisely for a no-reflash A/B), fresh capture → compare gate
  PASS → evaluate.
- All standard gates (§7).

---

## 6. PHASE 4 + 5 — timing (after Phase 3)

The baseline data says this matters more than originally weighted: at the
rate-correct operating point the offsets are median-0 but σ≈222 ms — the
detector fires on assorted onsets, not the beat grid. Two stages:

1. **Phase 4 — 50% overlap** (hop 256 → 16 ms ODF rate). Design already
   agreed: keep `audio_dsp_process()` a pure 512-window transform (preserves
   the whole test suite); add `audio_dsp_process_block()` stager inside
   audio_dsp (static 256-sample tail, 2 windows per DMIC block, returns up to
   2 results). `HISTORY_LEN` 32→64 (~1 s), refractory default 5→10, AGC
   counters stay in DMIC-block units, msgq rate doubles (drain-all consumer
   already copes). Halved hop halves flux magnitudes → floor/delta re-tune
   pass afterward (why Phase 4 comes after Phase 3 settles the threshold
   shape). GATE: only do this if the Phase-3 corpus results still show
   misses/jitter attributable to 32 ms quantization — check the offset
   histogram first.
2. **Phase 5 — tempo/beat-grid tracking** (the likely real fix for "in time
   with the music"): autocorrelation over a ~4 s ODF ring every ~250 ms,
   τ ∈ [37, 125] @ 31.25 Hz (200–60 BPM), log-Gaussian prior at 120 BPM;
   predicted-beat phase gates/aligns the raw detections. Design NOT yet
   detailed — write a design section (or mini plan doc) before implementing,
   and re-read `fw/docs/audio-fft.md` Phase 5 + the references there
   (Scheirer 1998, Ellis 2007). This is the largest remaining item; consider
   splitting induction (BPM estimate exposed via a characteristic — great
   observability win on its own) from phase-locking.

---

## 7. Standard gates & workflow (condensed; details in fw/CLAUDE.md)

Every PR: `west build --build-dir fw/build fw --board
rgb_sunglasses_proto0/nrf5340/cpuapp --sysbuild -- -DBOARD_ROOT="$(pwd)/fw"`
clean → `twister -T fw/tests -p native_sim --coverage --coverage-tool lcov
--outdir fw/twister-out` all green → patch coverage >70% via the pipeline in
`.claude/skills/submit-pr/references/patch-coverage.md` → `cd fw && pytest
tools/tests/` → device verification with music → PR via the `/submit-pr` house
style (measured numbers + explicit **not exercised** list). The repo's AI
review (`/code-review high`) has caught real, serious bugs on every PR so far
(9–10 findings each) — budget a fix round, take it seriously, reply per-thread
with `gh api .../comments/<id>/replies`.

Traps that repeatedly bit this effort (all now documented in fw/CLAUDE.md, but
the expensive ones):

- **Shell cwd persists across tool calls** — prefix every west/twister/pytest
  with `cd /workspaces/rgb-sunglasses &&`; verify artifact mtimes after
  "green" reruns (a stale `twister.json` once impersonated a passing run).
- **J-Link flash**: `fix-usb-dev-nodes.sh` before/after; first-attempt
  failures are NORMAL (retry loop); count TWO "flashed successfully" lines
  (netcore + appcore) — grepping one is how a stale appcore shipped once.
- **ttyACM numbering shifts after every reset**; recreate nodes from sysfs and
  re-open (the MCP serial connection goes stale too).
- **Persisted characteristics override compiled defaults** — on-device tests
  must write values via shell/BLE; the shared board carries α=1.5 + Phase-2
  targets already.
- **Host writes to the MSC disk require a board reboot** before firmware
  touches the FS again; free space is tight (~6.9 MB total, GLIM assets use
  ~2.1 MB) — delete old captures (after pulling them!) between long records.
- **hw-lock discipline**: `Monitor(scripts/hw-lock.sh hold board)` before any
  serial/flash/MSC work; release when the hardware loop is done.

## 8. Open follow-ups (small, non-blocking, do opportunistically)

1. `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE` was bumped 2048→3072 on headroom
   grounds; right-size with a measured high-water pass
   (`CONFIG_THREAD_ANALYZER` during a bond store).
2. CCC store at the new 96 ceiling has not been exercised with multiple bonded
   peers.
3. `record_wav`'s free-space guard counts an about-to-be-truncated existing
   file at the same path as used space (rejects overwrite-in-place that would
   actually fit).
4. The `rgb_sunglasses.sound_dump` MCP plugin tool works for long captures
   now, but unfrozen `record_wav` + sidecar CSV is the better instrument for
   AGC-dynamics observation (dump blocks the shell for the duration).
5. Quiet-regime flux-history caveat on `audio_dsp_compensate_gain_change`
   (documented in-code): revisit if quiet-intro false positives persist after
   Phase 3.
6. Near-clip fast path has never fired at real festival SPL (unit-tested
   only) — worth one deliberately-loud test someday.
7. The input-referred noise gate deliberately never chases sources below the
   gate at input (documented trade-off; `sound agc gate 0` = escape hatch).
   If the user reports "glasses don't react to quiet TV", this is why — it's
   a knob, not a bug.

## 9. Success criteria for issue #264 overall

The issue closes when, on a fresh music session: (a) quiet room → zero beat
flashes at parked gain (DONE, Phase 2); (b) music at any reasonable volume →
beats visibly locked to the music to a human observer, with corpus F-scores
materially above the 0.2–0.3 plateau at ±50 ms (Phases 3–5 territory); (c) all
behavior reproducible through the offline rig. The user's target event is
ABGT 700 — EDM-first tuning priority stands.
