# Beat detection & AGC (issue #264): Phase 3+ handoff plan

**Status date: 2026-08-02 (revised same day, after Phases 1-3 merged).** This document is a complete handoff for continuing
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
  matplotlib `report`). All firmware pieces gated by `CONFIG_APP_AUDIO_DEBUG`,
  which is now **default n** (reclaims a measured 33,440 B of appcore RAM,
  92.04% -> 84.62%). Rebuild with `-DCONFIG_APP_AUDIO_DEBUG=y` for a
  measurement session — see fw/docs/beat-detection-debugging.md.
- **PR #277 — Phase 1: gain-step compensation.** AGC gain steps no longer wipe
  the detector's 1 s adaptive-threshold history;
  `audio_dsp_compensate_gain_change(steps)` scales the linear-domain
  previous-frame energy instead (exact at all levels; `|steps| > 4` falls back
  to full reset). CRITICAL ordering contract (found by review, verified on
  hardware): compensation must be applied AFTER `audio_dsp_process()` consumes
  the last old-gain block — see the contract comment on the function and the
  `test_gain_compensation_misordered_is_harmful` ztest. On-device result:
  threshold-history retention across a gain step went from 3% to 98–102%.

- **PR #281 — Phase 3: threshold shape + alpha retune (MERGED).** Added a
  runtime threshold-mode switch (0 = `mean + α·σ`, 1 = `median + sf_delta`) and
  retuned the shipped `beat_alpha` **3.5 → 0.3**, which was the actual win —
  band-0 F roughly tripled. Mode 1 ships **defaulted off**: it wins per-clip but
  loses on the single-shared-setting criterion, because `sf_delta` is an
  absolute offset while per-band flux scales differ >20×. Also **rejected by
  measurement**: moving band 0 off bin 1 (cost F on every clip) and a floor
  retune (floor is provably inert). See §5's OUTCOME block.

### Open

- **Nothing is open.** Phases 1-3 are all merged. **Read §5.5 for what to do
  next** — the priority order changed after a field report.

- **PR #279 — Phase 2: AgcController (MERGED).** Extracts AGC decision logic
  into BT-free/Zephyr-free `fw/src/sound/agc_controller.{h,cpp}` compiled
  identically into firmware + unit suite (`fw/tests/sound/agc`) + replay
  harness. Policy: near-clip fast path (peak ≥ 32000 → −2 steps, no rate
  limit), attack (3 consecutive instantaneous-RMS frames > targetHigh → −1),
  release (15 consecutive smoothed frames < targetLow → +1, never while
  silent), **input-referred noise gate** (smoothed RMS normalized to 0 dB park
  gain; silence → hold gain, suppress all `beat[]` output, park to 0 dB after
  ~10 s). Targets derived from captures: targetLow 0.002, targetHigh 0.05,
  gate 0.001. Hardware-verified 4/4 tests (see §4 numbers). **That gate default
  is now known to be too high for normal-volume music — see §5.5.1.**

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

Runtime tunables (BLE `audio/` keys + `sound dsp` / `sound agc` shell), **as of
Phase 3**: `flux_gamma` 1000, `beat_flux_floor` 0.005 (provably inert — see §4),
`beat_alpha` **0.3** (retuned from 3.5 in Phase 3; the shared dev board also has
0.3 persisted), `beat_refractory_frames` 5, `sf_delta` 0.10 and
`threshold_mode` **0** (Phase 3, mode 1 = median+delta available but not
default), `agc_target_low` 0.002, `agc_target_high` 0.05,
`agc_rate_limit_frames` 10, `agc_attack_frames` 3, `agc_release_frames` 15,
`noise_gate_rms` 0.001 (**suspect — see §5.5.1**), plus 2 FFT-display values.
14 persistent characteristics under `audio/`.

**Persisted values override compiled defaults on a provisioned board.** After
any default change, set it explicitly via `sound dsp set` before testing, or you
will measure the old value.

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

### The capture corpus (updated after Phase 3)

A corpus now lives at **`fw/testdata/beat-corpus/`** — 5 clips recorded
2026-08-02 (`base60`, `loud30`, `newbase`, `quiet40`, `verify30`; see the table
in `fw/docs/beat-detection-debugging.md`). It is **gitignored by deliberate
decision of the repo owner**, so it survives across sessions on this machine but
is absent from every fresh checkout, and the numbers in this document are
re-derivable but not re-runnable by someone else. `phase3_table.py` checks for
the clips up front and points at the record workflow rather than failing deep in
a run.

If the directory is empty, re-record — it takes minutes with any music. Note
§5.5.2's prerequisite: the current 5 clips are too few (and too same-y — two are
the same track) to tune a tempo tracker.

Historical note: the original captures lived in a session scratchpad and were
lost, which is why this section used to say the corpus was ephemeral.
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

> **OUTCOME (2026-08-02, same day — read this before §5.1).** Phase 3 is
> implemented and measured. The central hypothesis did **not** hold, and three
> of the design items below were rejected by their own evidence. The plan text
> is kept intact for provenance; the corrections are:
>
> 1. **Median+delta (mode 1) is implemented but NOT the default.** It wins when
>    tuned per clip (F 0.321/0.296/0.376 vs mode 0's 0.294/0.294/0.362), but
>    loses on the criterion that actually governs a shipped device — one
>    setting for every venue: best shared `sf_delta`=0.26 gives worst-clip
>    F 0.274, vs mode 0's best shared `alpha`=0.3 at 0.289 (max regret 0.030 vs
>    0.010). Mechanism, found in the on-device A/B: `sf_delta` is an ABSOLUTE
>    flux offset, but per-band flux scales differ >20x (band 0 peaks ~3.5,
>    band 3 ~0.2) — at delta 0.10 band 3 fired 1x per 300 frames where mode 0
>    fired 37x. Alpha is scale-relative, which is why it transfers. A future
>    mode-1 default needs a per-band delta or a normalized flux.
> 2. **§5.1 item 7 (band 0 → bin 2) is WRONG — reverted.** Measured A/B on one
>    build: moving band 0 off bin 1 cost F on every clip (base60 0.294→0.234,
>    loud30 0.294→0.266, newbase 0.362→0.312). Bin 1 (31 Hz) carries real kick
>    energy; the display-bucket table's reasoning does not transfer.
> 3. **The actual win was retuning alpha 3.5 → 0.3**, which this plan never
>    considered because the earlier sweeps only covered alpha >= 1.0 and read
>    "best at the edge" as a result. Band-0 F: base60 0.014→0.294,
>    loud30 0.000→0.294, newbase 0.129→0.362. Quiet-room cost: none (0 beats
>    per 40 s at every alpha — the Phase 2 gate owns that regime).
> 4. **§5.1 item 6 (floor retune): not needed.** The floor is still provably
>    inert at the new operating point — identical fire counts across
>    0.005–0.105.
>
> See the PR for the full corpus tables; `fw/tools/beat_lab/phase3_table.py`
> regenerates them.

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

## 5.5 WHAT TO DO NEXT (added 2026-08-02, after Phase 3 merged)

**Phases 1-3 are merged** (PRs #277, #279, #281). Read this section before §6:
it re-orders the remaining work based on a field report plus measurements the
earlier phases could not have had.

### 5.5.1 ~~TOP PRIORITY~~ **DONE — see PR #283** — the noise gate was suppressing real music

> **RESOLVED (2026-08-02, PR #283).** Shipped as two default changes:
> `noise_gate_rms` **0.0010 → 0.0006** and `beat_flux_floor` **0.005 → 0.08**.
> The new configuration **dominates** the old one on every corpus clip — quiet
> music F 0.088 → 0.195, base60 0.282 → 0.291, loud30 0.280 → 0.291, newbase
> 0.293 → 0.348, and the quiet room got *quieter* (2 → 1 beats/40 s). Confirmed
> on hardware by the repo owner watching the Beat animation.
>
> Two findings worth carrying forward:
>
> 1. **The flux floor is no longer inert, and that is a consequence of Phase 3.**
>    §4 records it as provably inert (identical fire counts 0.005–0.105) — true
>    at alpha 3.5, false at 0.3. A lower adaptive threshold lets small
>    noise-flux events through, and an absolute, scale-fixed floor is the right
>    tool against them. Raising it to 0.08 cut quiet-room beats 4 → 1 per 40 s at
>    **zero** cost to any music clip; above 0.08 it starts clipping real beats.
>    A ztest (`test_flux_floor_rejects_small_onsets`) now pins this so the old
>    "the floor does nothing" conclusion can't be re-applied.
> 2. **The structural fix proposed below was evaluated and REJECTED.** Removing
>    beat suppression entirely (keeping only the gain hold) gives **219**
>    quiet-room beats — the flux threshold alone cannot reject room noise at
>    alpha 0.3. A separate, lower beat gate measured within noise of simply
>    lowering the single threshold, so it was not worth a second characteristic,
>    a `#PARAMS` field across six consumers, and another knob.
>
> The analysis below is kept as the record of how the problem was found.

### The original analysis

**Field symptom (reported by the repo owner, 2026-08-02):** with music playing
the glasses sometimes do not react at all, and turning the volume up fixes it.

This is NOT a threshold or timing problem. It is the Phase 2 input-referred
noise gate (`audio/noise_gate_rms`, default 0.0010) suppressing **all** `beat[]`
output. Measured over the corpus, as smoothed input-referred RMS — the exact
quantity the gate compares against:

| Clip | Condition | p5 | p50 | p95 | frames below the 0.0010 gate |
|---|---|---|---|---|---|
| `quiet40` | quiet room | 0.00013 | 0.00017 | 0.00049 | 98.3% (correct) |
| `verify30` | **music, normal volume** | 0.00061 | 0.00099 | 0.00114 | **52.9%** |
| `newbase` | music | 0.00092 | 0.00129 | 0.00248 | 11.3% |
| `base60` | music | 0.00094 | 0.00146 | 0.00170 | 7.5% |
| `loud30` | music, volume raised | 0.00299 | 0.00322 | 0.00354 | 0.0% |

The gate threshold sits **above the bottom half of real music**: quiet-room p95
is 0.00049 while music p5 is 0.00061-0.00094. Over half of a normal-volume
capture is gated off entirely, and raising the volume lifts the signal clear —
precisely the reported symptom. §8 item 7 predicted this trade-off ("glasses
don't react to quiet TV ... it's a knob, not a bug"); the field report says the
knob is simply set wrong for normal listening levels.

**Do this before any Phase 4/5 work.** It is a parameter change plus
measurement (hours, not days), it is fully measurable offline against the
existing corpus with `--agc sim`, and it addresses the actual complaint. Points
to consider while doing it:

- The obvious move is lowering `noise_gate_rms` to ~0.0006. Careful: room noise
  is NOT stationary across sessions (§4 records 0.00057-0.0016 input-referred on
  earlier nights, versus p95 0.00049 in `quiet40`). A single global threshold
  may not separate "quiet room" from "quiet music" at all — the distributions
  genuinely overlap.
- Better structural fix to evaluate: the gate currently does two jobs — (a) hold
  the AGC gain so it stops chasing room noise, and (b) suppress `beat[]` output.
  Only (a) needs to be conservative. Decoupling them (keep the gain hold, drop
  or greatly lower the beat suppression, and let the flux threshold + floor
  reject noise on their own) would likely fix quiet music without reopening the
  quiet-room false-beat problem Phase 2 solved. Measure both options against
  `quiet40` (must stay at 0 beats) and the music clips.
- Whatever ships, re-verify `quiet40` shows **zero** beats — that is Phase 2's
  success criterion and issue #264's criterion (a).

### 5.5.2 Phase 5 feasibility — prototyped offline, results mixed

Autocorrelation tempo induction was prototyped in Python against the corpus
before committing to firmware. Findings that change the §6 plan:

1. **§6's tau range is wrong for this hardware.** It specifies `tau in [37,125]`,
   which assumes a 125 Hz ODF (`fw/docs/audio-fft.md` Level 4 says the same).
   Our ODF runs at **31.25 Hz** (512-sample hop, no overlap), so the correct
   range is **`tau in [9, 31]`** for 200-60 BPM. Anyone building to the old
   numbers will search entirely the wrong band.
2. **It works, and 31.25 Hz is sufficient** — with parabolic interpolation of
   the autocorrelation peak, which is needed because integer lags quantize to
   7-8 BPM at EDM tempos. With a realistic 4 s window updating every 250 ms:
   `base60` 82% of windows within 5 BPM of truth, `loud30` 100%.
3. **The ACF peak height is a well-separated confidence signal**: 0.45-0.56 when
   the estimate is right, 0.17-0.18 when wrong. This is the piece §6 lacked —
   phase locking must be **gated** on it, falling back to current per-onset
   behavior when confidence is low. A wrong beat grid looks far worse to a human
   than no grid.
4. **One of three clips cannot be tempo-tracked at all.** `newbase`'s band-0 ACF
   is flat (top peaks 0.105/0.102/0.093; the true tau=12 is not even top-5). The
   120 BPM log-Gaussian prior was suspected and **exonerated** — removing it
   changes nothing. That clip's ODF simply is not periodic enough.

**Prerequisite before Phase 5:** expand the corpus. Three clips, two of which
are the same track, is too thin to tune a tempo tracker — finding 4 is exactly
how that bites. Target 6-8 clips spanning 100-160 BPM, genre, and kick
prominence.

**If Phase 5 proceeds, split it:**
- **5a — induction only.** ACF + interpolation + confidence, BPM and confidence
  exposed via a characteristic and `sound dsp`. **No change to beat firing.**
  Standalone observability win, fully testable offline, de-risks 5b.
- **5b — confidence-gated phase locking.** Below threshold, behave exactly as
  today.

### 5.5.3 Honest cost/benefit on Phase 5

Recorded so the next session does not re-litigate it:

- The F-score plateau (0.29-0.38) is **partly a measurement artifact**. The
  reference is librosa's beat tracker, not human annotation, and the window is
  +/-50 ms. `base60` scores F=0.29 while a human observer already describes beat
  matching as improved. Chasing F is not the same as chasing perceived quality,
  and nothing in this project has yet validated that they correlate here.
- Phase 5 is the **largest remaining item** by far, and 1 of 3 corpus clips
  cannot be tracked at all — so it would ship as "helps on strongly periodic
  music, inert otherwise".
- After Phase 3, the owner's remaining complaint was **not** timing — it was the
  detector not firing at all (see 5.5.1). Sensitivity, not phase.

**Recommendation: fix the gate (5.5.1), then re-assess perceptually before
starting Phase 5.** If the glasses look locked to the music once they stop
dropping out, Phase 5 may not be worth its cost. Decide with fresh eyes and, if
possible, a human A/B rather than an F-score.

> **UPDATE (2026-08-02):** the gate fix shipped (PR #283) and the repo owner's
> perceptual verdict on the result was **"this looks pretty good to me"** —
> assessed by watching the Beat animation with music, which is issue #264's
> actual criterion rather than the F-score proxy.
>
> **So Phase 5 is NOT currently justified.** It remains the largest open item,
> it helps only on strongly periodic music (1 of 3 corpus clips could not be
> tempo-tracked at all), and the symptom that motivated re-opening this work is
> now fixed by two parameter changes. Start Phase 5 only if a *new* observation
> says timing specifically is the problem — e.g. "it fires steadily but on the
> wrong beats" or "it drifts against the music" — not merely because the
> F-scores are still ~0.3. Nothing has established that F at ±50 ms tracks
> perceived quality on this device, and the one time both were measured they
> disagreed.

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

   > **GATE EVALUATED 2026-08-02: DO NOT DO PHASE 4 NEXT.** The offset
   > histogram at the tuned operating point (base60, mode 0, alpha=0.3) is:
   > median +64 ms, **sd 143 ms**, 24% of detections within ±50 ms but 98%
   > within ±250 ms; median inter-detection interval 288 ms against a 480 ms
   > true beat period (1.59 detections per beat). The scatter is ~4.5 frames —
   > an order of magnitude larger than the 32 ms quantization Phase 4 would
   > halve, so halving the hop cannot fix it. The detector is firing on
   > assorted onsets with no model of which are beats, and the report plot
   > shows the kick pattern is unmistakably periodic in RMS. Go straight to
   > Phase 5 (tempo/beat-grid tracking); revisit Phase 4 only if a phase-locked
   > detector then turns out to be resolution-limited.
2. **Phase 5 — tempo/beat-grid tracking** (the likely real fix for "in time
   with the music"): autocorrelation over a ~4 s ODF ring every ~250 ms,
   **τ ∈ [9, 31] @ 31.25 Hz** (200–60 BPM — the τ ∈ [37, 125] originally written
   here, and in `fw/docs/audio-fft.md` Level 4, assumes a 125 Hz ODF and is
   WRONG for this firmware; see §5.5.2), log-Gaussian prior at 120 BPM;
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
7. ~~The input-referred noise gate deliberately never chases sources below the
   gate at input (documented trade-off; `sound agc gate 0` = escape hatch).
   If the user reports "glasses don't react to quiet TV", this is why — it's
   a knob, not a bug.~~ **PROMOTED TO TOP PRIORITY — see §5.5.1.** This
   prediction came true in the field, but for *normal-volume music*, not just
   quiet TV: 52.9% of a normal-volume capture is below the gate. The knob is
   set wrong, and it is no longer a "non-blocking" follow-up.

## 9. Success criteria for issue #264 overall

The issue closes when, on a fresh music session: (a) quiet room → zero beat
flashes at parked gain (DONE, Phase 2); (b) music at any reasonable volume →
beats visibly locked to the music to a human observer, with corpus F-scores
materially above the 0.2–0.3 plateau at ±50 ms (Phases 3–5 territory); (c) all
behavior reproducible through the offline rig. The user's target event is
ABGT 700 — EDM-first tuning priority stands.
