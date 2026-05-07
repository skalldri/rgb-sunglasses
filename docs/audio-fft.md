# Beat detection on nRF5340: a phased implementation playbook

**The nRF5340 application core is wildly over-provisioned for beat detection** — a complete pipeline (PDM capture → 512-pt float FFT → spectral-flux onset detection → BPM tracking → WS2812 strip) consumes under 10% of the 128 MHz Cortex-M33F and well under 20 KB of RAM. The hard parts are not algorithmic; they are the platform plumbing: getting `dmic_nrfx_pdm` to deliver gap-free 16 kHz PCM into a `k_mem_slab`, persuading WS2812 drivers to behave on nRF53 (use the I2S backend, not SPI), and choosing an algorithm whose threshold logic survives across genres. All required building blocks exist in NCS v3.1.1 today; **no Nordic-published audio-reactive sample exists**, so this is greenfield integration work, but every piece has a known-working upstream reference. The practical recommendation is to ship Level-1 energy detection in a day, then jump straight to Level-3 log-magnitude spectral flux — Level-2 sub-band energy is a worse algorithm than Level-3 for similar code volume.

A note on the spec sheet: the prompt states 256 KB RAM, but the **nRF5340 application core actually has 512 KB SRAM and 1 MB flash**; the network core has 64 KB RAM and 256 KB flash. Either figure is comfortable, but the correction matters when budgeting for BLE stacks or future ML add-ons.

## The PDM-to-PCM capture pipeline is mostly done for you

On nRF5340 the PDM peripheral performs hardware decimation to 16-bit PCM, so the application core spends essentially zero CPU on the audio path itself — only a handful of IRQs per second to refill DMA buffers. The Zephyr `dmic_nrfx_pdm` driver wraps `nrfx_pdm` and exposes the standard `dmic_configure()` / `dmic_trigger()` / `dmic_read()` API from `<zephyr/audio/dmic.h>`. The canonical reference is the upstream sample at `samples/drivers/audio/dmic`, which already ships a verified `boards/nrf5340dk_nrf5340_cpuapp.overlay`. Clone it, change the block size, and you are done.

The mental model is: you allocate a `K_MEM_SLAB_DEFINE` of N PCM blocks; the driver internally ping-pongs between them via EasyDMA and pushes filled blocks onto an internal `k_msgq`; `dmic_read()` blocks on that msgq and returns a pointer the caller **must** later `k_mem_slab_free()`. Forgetting that free is the #1 reported failure mode (`-ENOMEM`, "No room in RX queue"). For beat detection use **10 ms blocks (160 samples at 16 kHz mono int16 = 320 bytes)** and 8 slab blocks for 80 ms of head-room. A block size of 1 ms is unnecessary on nRF5340 (the 1 ms floor is an artifact of the unrelated `mpxxdtyy` software driver).

The DTS overlay needs only a `pinctrl` group for `PDM_CLK` and `PDM_DIN`, plus `clock-source = "PCLK32M_HFXO"` for the simple case (or `"ACLK"` if you want a perfectly-rational 16/48 kHz family from the audio PLL, which then requires `hfclkaudio-frequency = <12288000>` on the `&clock` node). Required Kconfigs are minimal: `CONFIG_AUDIO=y`, `CONFIG_AUDIO_DMIC=y`, `CONFIG_AUDIO_DMIC_NRFX_PDM=y`. Caveat for hardware selection: the **nRF5340 Audio DK** wires its on-board mics through a CS47L63 codec on I2S, not directly to the SoC PDM, so prototype on the plain **nRF5340 DK** with an external PDM MEMS mic.

## CMSIS-DSP setup is one Kconfig away

The application core is a Cortex-M33**F** with FPU and DSP (SIMD/MAC) extensions — same DSP class as Cortex-M4F. It does **not** have Helium/MVE (M55+ only), so do not enable `ARM_MATH_MVEF`. Turn on `CONFIG_FPU=y`, `CONFIG_FPU_SHARING=y`, `CONFIG_CMSIS_DSP=y`, `CONFIG_CMSIS_DSP_TRANSFORM=y`, `CONFIG_CMSIS_DSP_COMPLEX_MATH=y`, `CONFIG_CMSIS_DSP_BASIC_MATH=y`. A known macro collision between Zephyr and `arm_math.h` (`ROUND_UP`/`ROUND_DOWN`) is documented as Zephyr issue #64327 — workaround is to include all Zephyr headers before `arm_math.h`.

**Use `arm_rfft_fast_f32`, not the deprecated `arm_rfft_f32`.** Always call the **length-specific initializer** like `arm_rfft_fast_init_512_f32()` rather than the generic init — this lets the linker discard twiddle tables for unused FFT sizes and saves several KB of flash. Output is in CMSIS's "packed Nyquist" format: bin 0 holds Re(DC) and Re(Nyquist) packed into the first complex slot, then bins 1..N/2-1 follow as standard interleaved complex pairs. For beat work, follow with `arm_cmplx_mag_squared_f32` on `&out[2]` (skipping the packed DC/Nyquist) — power spectrum is what you need and skipping the sqrt is roughly 2× faster than `arm_cmplx_mag_f32`.

**Choose 512-point Hann-windowed RFFT in float32 as the default.** At 16 kHz that gives a 32 ms window with 31.25 Hz bins — eight bins below 250 Hz, plenty for kick discrimination. Drop to 256 if you need finer time resolution for snares; go to 1024 only if you need true bass-pitch resolution (rare). Hann via `arm_hanning_f32(window, N)` precomputed once, applied with `arm_mult_f32` per frame. Q15 is roughly 2–2.5× faster but introduces packed-output and internal-scaling traps; only switch if profiling shows a CPU bottleneck, which it won't.

**Estimated FFT cost on nRF5340 @ 128 MHz** (extrapolated from ARM's M4F whitepaper and NXP's LPC5500 measurements — measure with `DWT->CYCCNT` once integrated):

| FFT size, f32 RFFT | Approx cycles | Wall time | CPU at 16 kHz hop=N/2 |
|---|---|---|---|
| 256 | ~10 k | ~80 µs | <1% |
| **512** | **~22 k** | **~180 µs** | **~1.2%** |
| 1024 | ~50 k | ~400 µs | ~1.3% |

Memory for a 512-point f32 pipeline: roughly 7–8 KB of RAM (input/scratch/output/magnitude/window) plus ~3.5 KB of flash twiddle tables.

## Algorithm progression: skip Level 2, go from energy to spectral flux

There are four useful complexity tiers, but the practical advice is to implement Level 1 first to validate the audio path, then jump to Level 3.

**Level 1 — Patin time-domain energy.** The seminal Frédéric Patin "Beat Detection Algorithms" article (2003, archived at flipcode.com/misc/BeatDetectionAlgorithms.pdf and on gamedev.net) defines the workhorse hobbyist algorithm: instant block energy E vs. local mean ⟨E⟩ over ~1 s of history, with adaptive threshold C(V) = −0.0025714·V + 1.5142857 fitted empirically (V is the variance of the energy history). Beat fires when E > C·⟨E⟩. **Patin's specific constants are tuned for 44.1 kHz, 1024-sample blocks; do not port them verbatim.** The cleaner, units-agnostic form used by Parallelcube and many others is `threshold = mean + α·σ` with α ∈ [1.5, 3.0]. At 16 kHz use B=512 sample blocks (32 ms) and M=31 history slots (~1 s). Cost: trivial, well under 1% CPU. Latency: ~32 ms. Strength: phenomenal on bass-heavy EDM/techno where Patin himself recommended C ≈ 1.4. Weakness: misses snares hidden under sustained bass.

**Level 2 — Patin frequency-selected sub-band energy.** Run an FFT, partition bins into S sub-bands (logarithmic spacing matches hearing better than linear), keep per-band history, beat-detect per band. For N=512 at 16 kHz with Δf=31.25 Hz: bass kick = bins 1–6 (40–200 Hz), snare body = bins 6–26 (200–800 Hz), snare snap = bins 26–64 (800–2000 Hz), hi-hat = bins 64–192 (2–6 kHz). The lwbd library on GitHub (qlyoung/lwbd) is a clean C reference. **The blunt truth: Level 2 is only marginally more code than Level 3 but vastly less robust** — Bello/Dixon comparisons consistently show that simple sub-band energy underperforms log-magnitude spectral flux on anything other than four-on-the-floor electronic music. Use Level 2 only if you specifically want independent per-band beat events for color mapping.

**Level 3 — Log-magnitude spectral flux.** This is the modern non-deep-learning standard and the algorithm to actually deploy. The reference is **Bello et al., "A Tutorial on Onset Detection in Music Signals" (IEEE TSAP 2005)**, with refinements in **Dixon, "Onset Detection Revisited" (DAFx-06)**. The detection function is the half-wave-rectified frame-to-frame difference of log-compressed magnitude spectra, optionally aggregated into 24 mel bands:

```
S_n[m] = log(1 + γ · Σ_k W_m[k] · |X_n[k]|),    γ ≈ 1000
SF(n)  = Σ_m max(0, S_n[m] − S_{n-1}[m])
```

Followed by adaptive-threshold peak picking — running median over ~1 s plus a minimum inter-onset interval of 30–50 ms. **Böck & Widmer's SuperFlux (DAFx-13)** adds a max-filter trajectory tracker over a small frequency neighborhood with a 2-frame lag, suppressing vibrato false positives; it is the strongest classical ODF still in use. Cost on nRF5340: ~5% CPU at 125 Hz hop rate. Latency: 30–60 ms including 1–3 frames of peak-picking look-ahead. Memory working set: ~14 KB.

**Level 4 — Tempo estimation via autocorrelation or comb filters.** Once you have the ODF d(n) at fr ≈ 125 Hz, BPM follows from periodicity. The autocorrelation method computes R(τ) = Σ d(n)·d(n+τ) over a 4-second window for τ ∈ [37, 125] samples (200 → 60 BPM at fr=125 Hz), weighted by a log-Gaussian prior centered at 120 BPM (σ ≈ 0.3 octaves, per Klapuri 2006) to break octave ambiguity. Cost: ~45k MACs every 250 ms, trivial. The alternative is **Scheirer's 1998 comb-filter resonator bank** (JASA 103(1):588), running ~60 parallel comb filters tuned to candidate tempos and picking the resonator with highest output energy; this is the foundational paper and produces smoother tracking but uses more state. For phase tracking (when does the *next* beat occur?), Ellis 2007's dynamic-programming beat tracker is the standard.

## The implementations worth mining

Six existing projects deserve direct study, ordered by relevance to the nRF5340 port:

The **Teensy Audio Library** (PaulStoffregen/Audio, MIT) is the closest publicly available code base to what you need — it already wraps `arm_cfft_radix4_q15` and `arm_cfft_radix4_init_q15` in `analyze_fft256.h` and `analyze_fft1024.h`, runs on Cortex-M4F/M7, and pairs naturally with two community beat-detection sketches: `gibbedy/BeatDetector` (per-band running average + 1 s peak-max with retrigger lockout, ~150 lines, MIT-style) and `indicatesvoid/BeatDetectionTeensy` (peak-only with decay floor and minimum inter-beat interval, mirroring Java's Minim library). The class structure ports cleanly: replace `AudioStream` with the Zephyr DMIC msgq.

**WLED-MM** (MoonModules/WLED-MM, EUPL-1.2) is the flagship audio-reactive LED firmware on ESP32. Its `usermods/audioreactive/audio_reactive.cpp` (~3000 lines) implements bin-energy beat detection with a `peakDelay` lockout, a sophisticated AGC with three presets, a 16-band GEQ derived from arduinoFFT, and a "PinkNoise" equalization curve to flatten the music spectrum. The default beat detection uses bin 14 (~280 Hz) > moving average + threshold. **Replace `arduinoFFT` with `arm_rfft_fast_f32` and the bin-threshold logic ports in roughly 30 lines.**

**BTrack** (adamstark/BTrack, GPLv3) is the algorithmic gold standard for real-time beat tracking. Implements complex spectral difference ODF, comb-filterbank tempo induction, and DP cumulative scoring. **Beware GPLv3 if shipping a closed-source product.** The `OnsetDetectionFunction::complexSpectralDifference()` method is the most useful single function to port — drop FFTW for `arm_rfft_fast_f32`.

**Damian Peckett's Arduino beat detector** (projecthub.arduino.cc/mamifero/arduino-beat-detector-881c72) is the cheapest possible kick detector: 5 kHz sampling → 200 Hz Chebyshev IIR LPF → full-wave rectifier → 1.5 Hz envelope LPF → adaptive threshold. ~80 lines, no FFT. Port directly to `arm_biquad_cascade_df1_f32`. Excellent for low-power Level-1 implementation when only kick detection is required.

**Michael Krzyzaniak's Beat-and-Tempo-Tracking** (github.com/michaelkrzyzaniak/Beat-and-Tempo-Tracking) is a cleanly structured C library implementing spectral-flux ODF + LPF + adaptive threshold, with `BTT_SUGGESTED_SPECTRAL_FLUX_STFT_LEN = 1024`. More portable than aubio (which is too heavy at ~50K SLOC and malloc-heavy, but whose individual files like `src/onset/peakpicker.c` and `src/spectral/specdesc.c` are educational, GPLv3).

The **Pixelblaze sensor board** (simap/pixelblaze_sensor_board, MIT) proves the ADC→DMA→circular-buffer→FFT pipeline works on a Cortex-M0 with no FPU and 8 KB RAM — `src/main.c` is a single well-commented file showing the pattern that scales directly to nRF5340 (where you'd swap the integer FFT for CMSIS-DSP float).

For Nordic-specific prior art, the closest existing work is the Hackster project "Stereo System Spectrum Analyzer using Nordic Audio DK" (LVGL spectrum visualization on nRF5340 Audio DK using LE Audio + LC3) and "Tremor health analytics with nRF5340 DK" (CMSIS-DSP FFT on SAADC input). **Neither implements beat detection** — that piece is genuinely net-new on this platform.

## LED output: use I2S, not SPI

Zephyr's `led_strip` API is straightforward (`led_strip_update_rgb(dev, pixels, num_pixels)`), with backends for WS2812 over SPI, I2S, and bit-banged GPIO, plus separate drivers for APA102/SK9822 and others. **For nRF5340 use the I2S backend** (`compatible = "worldsemi,ws2812-i2s"`). The SPI backend has documented timing glitches on nRF53 (Zephyr issues #29877 and #57147; DevZone thread 105200), and the GPIO bit-bang is Cortex-M0-only. The I2S backend encodes each WS2812 bit as a 4-bit nibble (`nibble-one = <14>`, `nibble-zero = <8>` in DT). You will need a 3.3→5 V level shifter and a series resistor on the data line for reliable signaling on real hardware.

WS2812B's 800 kbit/s wire protocol is hard-locked at **30 µs per LED** plus a 50 µs reset. That gives concrete frame-rate ceilings: 60 LEDs in 1.85 ms (target 120 Hz), 144 LEDs in 4.4 ms (100 Hz), 300 LEDs in 9 ms (60 Hz), 1024 LEDs in 30 ms (30 Hz). For long strips with high update rates use APA102/SK9822 instead, which clock from 1 to 20+ MHz SPI. The legacy sample path was renamed from `samples/drivers/led_ws2812` to `samples/drivers/led/led_strip` in recent Zephyr/NCS — use the new one as the build template.

For visual mapping, the consensus pattern is exponential smoothing with peak-hold falloff, which eliminates flicker without adding perceptible lag:

```c
smooth[k] = β · new[k] + (1-β) · smooth[k];   // β ≈ 0.3
peak[k]   = max(peak[k] · γ, smooth[k]);      // γ ≈ 0.95 per frame
```

Three classic visualization mappings cover most aesthetics: bass→R / mids→G / highs→B (sub-band-to-color); spectral centroid → HSV hue with total RMS → value (smooth rainbow that follows timbre); or palette-based brightness modulation where bass envelope drives a global brightness multiplier and color comes from a slowly rotating palette (most pleasant for casual listening). FastLED's `ColorFromPalette` and the cpt-city palette collection (via PaletteKnife) are the de facto standard; for Zephyr without FastLED, ship a 256-entry RGB LUT in flash with linear interpolation.

## System architecture: three threads on the app core, period

Run everything on the application core. The network core has no FPU, runs at fixed 64 MHz, has 64 KB RAM, and exists to host the radio stack — using it for DSP would force soft-float and lose the entire DSP-extension advantage. The PDM peripheral lives on the app-core domain anyway, and cross-core access via the bus bridges adds latency. Reserve the network core for BLE if you later add a "tempo over BLE" feature.

The recommended thread layout is three threads coordinated by Zephyr primitives. The DMIC driver's internal ISR refills DMA via `nrfx_pdm`. A **DSP thread** at cooperative priority `K_PRIO_COOP(7)` with a 4 KB stack blocks on `dmic_read()`, performs window→FFT→magnitude→ODF→peak-pick, frees the slab block, and pushes a `{float bands[8]; bool onset; uint32_t seq;}` struct to a `k_msgq`. An **LED thread** at preemptible priority `K_PRIO_PREEMPT(10)` consumes the queue (drop-oldest semantics for freshness), applies smoothing/decay, calls `led_strip_update_rgb()`, and sleeps to maintain 60–120 fps. Use `CONFIG_FPU_SHARING=y` because the DSP thread uses the FPU; do not use a `k_work_q` for this pipeline (work-queues add latency and are designed for bursty handlers).

Total system budget: roughly 13–18 KB RAM working set including the LED framebuffer for typical strip lengths, well under 10% of the 512 KB app-core SRAM. CPU load is dominated by the FFT (~0.2 ms per frame) and runs around 5–8% at 100 Hz hop rate, leaving enormous headroom for BLE Audio if you eventually add it.

End-to-end latency budget: PDM block (10 ms) + FFT/ODF (<1 ms) + peak-pick look-ahead (8–24 ms for Level 3, zero for Level 1) + LED transmit (under 5 ms for strips up to 144 LEDs) ≈ **20–40 ms**, comfortably under the ~50 ms threshold where humans start to perceive audio-light desynchronization.

## Phased implementation roadmap

The recommended sequence trades risk for velocity. Each phase produces a working artifact suitable for stopping if requirements are met.

**Phase 0 — Audio path bring-up (0.5 day).** Clone Zephyr's `samples/drivers/audio/dmic`, point its overlay at your PDM mic pins, enable `CONFIG_AUDIO_DMIC_NRFX_PDM=y`, verify 16 kHz int16 PCM blocks arrive in `dmic_read()`. Compute and log the RMS of each block to confirm the mic responds to sound. This validates the slab pattern and rules out hardware/DTS issues before any DSP is added.

**Phase 1 — Energy-based bass detection (1 day).** Implement Patin's time-domain algorithm in the cleaned-up `μ + 2σ` form on 32 ms blocks with a 31-block (~1 s) history. Drive a single LED on/off via GPIO on detection. **This will look great on EDM/techno** and validates the end-to-end timing. You can ship this as a v0 product.

**Phase 2 — CMSIS-DSP integration and sub-band energies (1–2 days).** Add `CONFIG_FPU=y`, `CONFIG_FPU_SHARING=y`, `CONFIG_CMSIS_DSP=y`, `CONFIG_CMSIS_DSP_TRANSFORM=y`, `CONFIG_CMSIS_DSP_COMPLEX_MATH=y`. Move processing into a dedicated DSP thread fed by the slab. Implement 512-point Hann-windowed `arm_rfft_fast_f32` followed by `arm_cmplx_mag_squared_f32`. Aggregate into 4–8 logarithmic sub-bands. Verify FFT timing with `DWT->CYCCNT`.

**Phase 3 — LED strip integration (1 day).** Wire the WS2812 I2S backend with a level-shifter, build from `samples/drivers/led/led_strip`, drive the strip from a separate LED thread fed via `k_msgq` from the DSP thread. Implement bass-driven brightness with exponential decay and a slowly rotating color palette. This is the first version that looks like a real product.

**Phase 4 — Spectral flux ODF (2–3 days).** Replace the per-band threshold with proper log-magnitude spectral flux across a 24-band mel filterbank (precomputed sparse weight pairs). Add adaptive median-thresholded peak picking with 3-frame look-ahead. **This is the upgrade that makes the system work on rock, jazz, and acoustic music**, not just EDM. Latency rises from ~30 ms to ~50 ms but the quality improvement is qualitative.

**Phase 5 — Tempo tracking (2–3 days).** Maintain a 4 s ring buffer of the ODF, run autocorrelation over τ ∈ [37, 125] every 250 ms with a log-Gaussian BPM prior centered at 120 BPM, expose current BPM and beat phase. Optional: add Ellis-style DP beat-time prediction so visual effects can pre-trigger a few ms ahead of the beat for perfect sync. Optional: switch to Scheirer's comb-filter bank if BPM tracking is unstable.

## Bottom line

The platform fits the problem with two orders of magnitude of margin: a 512-point f32 RFFT runs in 180 µs on a 128 MHz Cortex-M33F, mel filterbanks add microseconds, and the entire DSP pipeline plus a 144-LED WS2812 strip fits in under 20 KB of RAM. The path of least regret is to layer complexity in clear phases — Patin energy → CMSIS-DSP FFT → log-mag spectral flux → autocorrelation tempo — pulling implementation patterns from the Teensy Audio Library (CMSIS-DSP usage), WLED-MM (post-FFT pipeline and AGC), BTrack (complex spectral difference math), and Zephyr's own `samples/drivers/audio/dmic` and `samples/drivers/led/led_strip` (platform glue). The two non-obvious pitfalls are the WS2812 SPI-backend instability on nRF53 (use I2S) and the Patin threshold constants being tied to 44.1 kHz units (use the units-agnostic `μ + α·σ` form instead). Everything else is well-trodden ground with public reference code.