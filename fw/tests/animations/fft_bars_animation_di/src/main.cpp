#include <animations/animation_renderer.h>
#include <animations/fft_bars_animation.h>
#include <zephyr/ztest.h>

namespace {
/* 4 test display buckets.
 * Test display is 8 px wide; halfWidth=4, kBarWidthPx=1 → 4 buckets per side.
 * Left:  bucket 0 → x=0, bucket 1 → x=1, bucket 2 → x=2, bucket 3 → x=3
 * Right (mirror): bucket 3 → x=4, bucket 2 → x=5, bucket 1 → x=6, bucket 0 → x=7 */
class MutableAudioSource : public AnimationAudioSource {
   public:
    /* One new analysis frame per tick by default (the ~30 Hz reality); tests can
     * freeze the stream (0) or batch frames to exercise frame-count-driven EMA. */
    void update() override { frameCount_ += framesPerUpdate_; }

    uint32_t frameCount() const override { return frameCount_; }

    void setFramesPerUpdate(uint32_t frames) { framesPerUpdate_ = frames; }

    /* Beat bands — not used by FftBarsAnimation, return stubs. */
    size_t numBands() const override { return 0; }
    float getBandEnergy(size_t) const override { return 0.0f; }
    bool isBeat(size_t) const override { return false; }

    size_t numDisplayBuckets() const override { return kTestBuckets; }

    float getDisplayBucketEnergy(size_t bucket) const override {
        return (bucket < kTestBuckets) ? energy_[bucket] : 0.0f;
    }

    void setEnergy(size_t bucket, float energy) {
        if (bucket < kTestBuckets)
            energy_[bucket] = energy;
    }

    void resetAll() {
        for (size_t b = 0; b < kTestBuckets; b++)
            energy_[b] = 0.0f;
    }

    /* 4 buckets × 1 px × 2 sides = 8 px — fills the test display exactly. */
    static constexpr size_t kTestBuckets = 4;

   private:
    float energy_[kTestBuckets] = {};
    uint32_t frameCount_ = 0;
    uint32_t framesPerUpdate_ = 1;
};

/* A scriptable FftVisualizationConfigSource. Defaults to the table values, so a test
 * that installs it unchanged must render exactly like one that cleared the source. */
class FakeFftConfig : public FftVisualizationConfigSource {
   public:
    float smoothing = 0.3f;
    float energyScale = 20.0f;
    float floorDb = -36.0f;
    float rangeDb = 36.0f;
    float tilt = 3.0f;

    float getSmoothingCoeff() const override { return smoothing; }
    float getEnergyScale() const override { return energyScale; }
    float getFloorDb() const override { return floorDb; }
    float getRangeDb() const override { return rangeDb; }
    float getTiltDbPerOctave() const override { return tilt; }
};

/* The window the EMA-step tests use: floor −40 / range 40 / no tilt, so a bucket power
 * maps to a height that is easy to reason about (E = 7.6e-4 → −31.2 dB → 0.22). */
FakeFftConfig makeStepWindow() {
    FakeFftConfig cfg;
    cfg.floorDb = -40.0f;
    cfg.rangeDb = 40.0f;
    cfg.tilt = 0.0f;
    return cfg;
}

/* With the step window, one 4-row test display and coefficient 0.3, this energy's
 * target height 0.2202 gives: 1 step → 0.066 (0.26 rows → 0), 2 steps → 0.112 (0.45 → 0),
 * 3 steps → 0.145 (0.58 → 1 row). So the bottom pixel lights iff all three of a frame's
 * steps have run — the property the proration tests below pin. */
constexpr float kStepEnergy = 7.6e-4f;

/* 8 × 4 display. */
static constexpr size_t kTestWidth = 8;
static constexpr size_t kTestHeight = 4;

struct PixelColor {
    uint8_t r = 0, g = 0, b = 0;
    bool isBlack() const { return r == 0 && g == 0 && b == 0; }
};

PixelColor sPixels[kTestWidth][kTestHeight];

class CapturingTestRenderer : public AnimationRenderer {
   public:
    size_t displayWidth() const override { return kTestWidth; }
    size_t displayHeight() const override { return kTestHeight; }
    void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override {
        if (x < kTestWidth && y < kTestHeight) {
            sPixels[x][y] = {r, g, b};
        }
    }
};

void resetCapture() {
    for (size_t x = 0; x < kTestWidth; x++) {
        for (size_t y = 0; y < kTestHeight; y++) {
            sPixels[x][y] = {};
        }
    }
}

bool allPixelsDark() {
    for (size_t x = 0; x < kTestWidth; x++) {
        for (size_t y = 0; y < kTestHeight; y++) {
            if (!sPixels[x][y].isBlack())
                return false;
        }
    }
    return true;
}

/* With kBarWidthPx=1: left column = bucket, right mirror = (W-1-bucket). */
bool bucketIsDarkBothSides(size_t bucket) {
    size_t leftX = bucket;
    size_t rightX = kTestWidth - 1 - bucket;
    for (size_t y = 0; y < kTestHeight; y++) {
        if (!sPixels[leftX][y].isBlack())
            return false;
        if (!sPixels[rightX][y].isBlack())
            return false;
    }
    return true;
}

/* Lit rows in a column, counted from the bottom (bars grow upward). */
size_t barRows(size_t x) {
    size_t rows = 0;
    for (size_t y = 0; y < kTestHeight; y++) {
        if (!sPixels[x][y].isBlack())
            rows++;
    }
    return rows;
}

/* The animation is a Meyer's singleton, so a config source installed by one test would
 * leak into the next. Every test starts from the constexpr fallbacks. */
void clearConfigBeforeEach(void *) { FftBarsAnimation::getInstance()->clearConfigSource(); }
}  // namespace

ZTEST_SUITE(fft_bars_animation_di_tests, NULL, NULL, clearConfigBeforeEach, NULL, NULL);

/* Zero energy → entire display dark. */
ZTEST(fft_bars_animation_di_tests, test_zero_energy_is_dark) {
    MutableAudioSource audio;
    audio.resetAll();

    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);

    zassert_true(allPixelsDark(), "Display should be fully dark with zero energy");
}

/* Non-zero energy for bucket 0 lights the bottom pixel of both its left column
 * (x=0) and its right mirror column (x=7). With the default window, E = 0.1 is
 * −10 dB → target 0.72; one 16 ms tick runs one step → 0.22 → one lit row. */
ZTEST(fft_bars_animation_di_tests, test_energy_lights_both_mirror_sides) {
    MutableAudioSource audio;
    audio.resetAll();
    audio.setEnergy(0, 0.1f);

    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);

    size_t bottomRow = kTestHeight - 1;
    zassert_false(sPixels[0][bottomRow].isBlack(), "Left x=0 bottom should be lit");
    zassert_false(sPixels[7][bottomRow].isBlack(), "Right mirror x=7 bottom should be lit");
}

/* Energy only in bucket 2 lights x=2 and its mirror x=5; others stay dark. */
ZTEST(fft_bars_animation_di_tests, test_bucket_column_isolation) {
    MutableAudioSource audio;
    audio.resetAll();
    audio.setEnergy(2, 0.1f);

    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 16);

    size_t bottomRow = kTestHeight - 1;
    zassert_false(sPixels[2][bottomRow].isBlack(), "Left x=2 bottom should be lit");
    zassert_false(sPixels[5][bottomRow].isBlack(), "Right mirror x=5 bottom should be lit");

    zassert_true(bucketIsDarkBothSides(0), "Bucket 0 (both sides) should be dark");
    zassert_true(bucketIsDarkBothSides(1), "Bucket 1 (both sides) should be dark");
    zassert_true(bucketIsDarkBothSides(3), "Bucket 3 (both sides) should be dark");
}

/* Bottom lit pixel is greenish; top lit pixel is reddish; right mirror matches left.
 * E = 1.0 is exactly the default window's ceiling (0 dB), so the bar converges to full. */
ZTEST(fft_bars_animation_di_tests, test_gradient_bottom_green_top_red) {
    MutableAudioSource audio;
    audio.resetAll();
    audio.setEnergy(0, 1.0f);

    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->init();

    CapturingTestRenderer renderer;
    for (int i = 0; i < 20; i++) {
        resetCapture();
        anim->tick(renderer, 16);
    }

    size_t bottomRow = kTestHeight - 1;
    zassert_equal(sPixels[0][bottomRow].r, 0, "Bottom pixel red should be 0");
    zassert_equal(sPixels[0][bottomRow].b, 0, "Bottom pixel blue should be 0");
    zassert_true(sPixels[0][bottomRow].g > 200, "Bottom pixel should be greenish");

    zassert_equal(sPixels[0][0].r, 255, "Top pixel red should be 255");
    zassert_equal(sPixels[0][0].g, 0, "Top pixel green should be 0");
    zassert_equal(sPixels[0][0].b, 0, "Top pixel blue should be 0");

    /* Right mirror must match left. */
    zassert_equal(sPixels[7][bottomRow].r, sPixels[0][bottomRow].r,
                  "Right mirror bottom should match left");
    zassert_equal(sPixels[7][0].r, sPixels[0][0].r, "Right mirror top should match left");
}

/* init() resets the smoothed heights and targets; display returns to dark immediately. */
ZTEST(fft_bars_animation_di_tests, test_init_resets_smoothed_energy) {
    MutableAudioSource audio;
    audio.resetAll();
    audio.setEnergy(0, 0.1f);

    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->init();

    CapturingTestRenderer renderer;
    for (int i = 0; i < 10; i++) {
        resetCapture();
        anim->tick(renderer, 16);
    }
    zassert_false(sPixels[0][kTestHeight - 1].isBlack(),
                  "Bottom pixel should be lit after energy builds up");

    audio.setEnergy(0, 0.0f);
    anim->init();

    resetCapture();
    anim->tick(renderer, 16);
    zassert_true(allPixelsDark(),
                 "Display should be dark immediately after init() with zero energy");
}

/* Issue #376: the EMA advances per NEW analysis frame, not per render tick — with
 * no new frame the bars must hold perfectly still no matter how many ticks pass. */
ZTEST(fft_bars_animation_di_tests, test_bars_freeze_without_new_frames) {
    MutableAudioSource audio;
    audio.resetAll();
    audio.setEnergy(0, 0.1f);

    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->init();

    CapturingTestRenderer renderer;
    for (int i = 0; i < 10; i++) {
        resetCapture();
        anim->tick(renderer, 16);
    }
    size_t bottomRow = kTestHeight - 1;
    zassert_false(sPixels[0][bottomRow].isBlack(), "Bottom pixel lit after energy builds up");

    /* Freeze the analysis stream. Steps already OWED for delivered frames may
     * still drain from the proration pool for a few ticks (target unchanged, so
     * the bar holds); drain them, THEN drop the source energy to zero and assert
     * stillness — with no new frame the target is not even recomputed. */
    audio.setFramesPerUpdate(0);
    for (int i = 0; i < 8; i++) {
        anim->tick(renderer, 33);  // drain any pending steps toward the same target
    }
    audio.setEnergy(0, 0.0f);
    for (int i = 0; i < 10; i++) {
        resetCapture();
        anim->tick(renderer, 33);
    }
    zassert_false(sPixels[0][bottomRow].isBlack(),
                  "Bars must hold still across ticks that deliver no new frame");
}

/* Issue #376: one new frame deposits kEmaStepsPerFrame=3 steps (the historical
 * ~3-ticks-per-frame response at 90 Hz), and a 33 ms tick's proration allowance
 * withdraws all 3. With kStepEnergy under the step window, 3 steps give a
 * height of 0.145 → one lit row; a single step (the old per-tick EMA at 30 Hz)
 * would give 0.066 → dark. */
ZTEST(fft_bars_animation_di_tests, test_one_frame_advances_three_ema_steps) {
    MutableAudioSource audio;
    audio.resetAll();
    audio.setEnergy(0, kStepEnergy);
    FakeFftConfig cfg = makeStepWindow();

    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->setConfigSource(cfg);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 33);  // one new frame; dt covers a full 32 ms allowance

    size_t bottomRow = kTestHeight - 1;
    zassert_false(sPixels[0][bottomRow].isBlack(),
                  "3 EMA steps per frame must light the bottom pixel in one tick");
    zassert_true(sPixels[0][bottomRow - 1].isBlack(),
                 "Bar must be exactly one pixel tall after one kStepEnergy frame");
}

/* PR #378 review round 6 (reviewer-authored, adopted verbatim): at the
 * still-supported 90 Hz render rate (dt=11) a frame's 3 EMA steps must spread
 * across the ~3 ticks the 32 ms frame spans, not burst on the arrival tick and
 * freeze on the other two. kStepEnergy straddles the 1-pixel threshold:
 * 1 step → 0.066 of the panel (dark), 3 steps → 0.145 (one pixel). This is the
 * ONLY test in the suite that fails if the proration is reverted to the
 * burst-all-owed-steps behavior it replaced (mutation-verified by the
 * reviewer: 10 of 10 -> 9 of 10 with exactly this test red). */
ZTEST(fft_bars_animation_di_tests, test_frame_steps_prorate_across_fast_ticks) {
    MutableAudioSource audio;
    audio.resetAll();
    audio.setEnergy(0, kStepEnergy);
    audio.setFramesPerUpdate(1);
    FakeFftConfig cfg = makeStepWindow();

    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->setConfigSource(cfg);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 11);  // one frame arrives; only ~1 step is due yet
    audio.setFramesPerUpdate(0);

    zassert_true(sPixels[0][kTestHeight - 1].isBlack(),
                 "Arrival tick must run ~1 step, not burst all 3");

    resetCapture();
    anim->tick(renderer, 11);
    resetCapture();
    anim->tick(renderer, 11);

    zassert_false(sPixels[0][kTestHeight - 1].isBlack(),
                  "The frame's remaining owed steps must drain on the ticks it spans");
}

/* PR #378 review round 6 (reviewer-authored, adopted verbatim): a wrapping
 * frame-count delta (0x55555556 * kEmaStepsPerFrame wraps uint32 to 2) must
 * still run the capped steps — this pins the cap's PLACEMENT before the
 * multiply. A post-multiply cap yields 2 EMA steps where the pre-multiply cap
 * yields the allowance-limited 3, and at kStepEnergy the two straddle the
 * 1-pixel bar threshold (2 steps → 0.112 → dark, 3 → 0.145 → one row). */
ZTEST(fft_bars_animation_di_tests, test_frame_delta_wrap_does_not_starve_ema) {
    MutableAudioSource audio;
    audio.resetAll();
    audio.setEnergy(0, kStepEnergy);
    audio.setFramesPerUpdate(0x55555556u);
    FakeFftConfig cfg = makeStepWindow();

    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->setConfigSource(cfg);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 33);

    zassert_false(sPixels[0][kTestHeight - 1].isBlack(),
                  "A wrapping frame-count delta must still run the capped steps");
}

/* COVERAGE-ONLY (PR #378 review; arithmetic corrected round 6): this executes
 * the catch-up-cap path with a huge frame delta, but the cap's EFFECT is not
 * observable through the render. With proration, this tick deposits 12 steps
 * (delta 1000 capped at kMaxCatchupFrames=4 frames x 3) yet WITHDRAWS only the
 * dt=33 allowance of 3, leaving 9 owed — and E = 1.0 is the window ceiling, so
 * even a single step toward it lights a row and the assertion cannot
 * distinguish step counts or cap placements. The wrap-safety property is
 * pinned by test_frame_delta_wrap_does_not_starve_ema above, not by this
 * assertion. */
ZTEST(fft_bars_animation_di_tests, test_frame_catchup_is_bounded) {
    MutableAudioSource audio;
    audio.resetAll();
    audio.setEnergy(0, 1.0f);
    audio.setFramesPerUpdate(1000);

    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);
    anim->init();

    CapturingTestRenderer renderer;
    resetCapture();
    anim->tick(renderer, 33);  // delta 1000 -> capped to 4 frames' worth of steps

    zassert_false(sPixels[0][kTestHeight - 1].isBlack(),
                  "Capped catch-up must still converge the bar");
}

/* ── The dB-window mapping (2026-09-03) ──────────────────────────────────── */

/* Render bucket 0 at `energy` for `ticks` 33 ms ticks and return its bar height in rows. */
static size_t renderRows(FftBarsAnimation *anim, MutableAudioSource &audio, float energy,
                         int ticks) {
    audio.resetAll();
    audio.setEnergy(0, energy);
    anim->init();
    CapturingTestRenderer renderer;
    for (int i = 0; i < ticks; i++) {
        resetCapture();
        anim->tick(renderer, 33);
    }
    return barRows(0);
}

/* The installed config source's window is what maps power to height: E = 0.1 lights a
 * row under the defaults (−10 dB in a −36..0 window) but is below a floor of 0 dB. */
ZTEST(fft_bars_animation_di_tests, test_config_source_window_applied) {
    MutableAudioSource audio;
    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);

    zassert_true(renderRows(anim, audio, 0.1f, 20) >= 1, "defaults must light E = 0.1");

    FakeFftConfig cfg;
    cfg.floorDb = 0.0f;
    anim->setConfigSource(cfg);
    zassert_equal(renderRows(anim, audio, 0.1f, 20), 0u,
                  "a floor at 0 dB must leave −10 dB dark");
}

/* Power below the floor never lights a bar, however long it is held. */
ZTEST(fft_bars_animation_di_tests, test_below_floor_is_dark) {
    MutableAudioSource audio;
    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);

    zassert_equal(renderRows(anim, audio, 1e-5f, 30), 0u, "−50 dB is below the −36 dB floor");
    zassert_true(allPixelsDark(), "no other column may light either");
}

/* An over-ceiling spike contributes exactly 1.0 to the EMA — the same as a signal
 * sitting exactly AT the ceiling — so its decay afterwards is identical. This is the
 * property the old linear mapping lacked: 0.25 × 20 = 5.0 kept the bar pinned for
 * several steps after the spike. */
ZTEST(fft_bars_animation_di_tests, test_ceiling_clamps_and_has_no_overshoot_memory) {
    MutableAudioSource audio;
    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);

    const float levels[] = {1000.0f, 1.0f}; /* +30 dB (spike) vs 0 dB (ceiling) */
    size_t decay[2][6];
    for (int run = 0; run < 2; run++) {
        audio.resetAll();
        audio.setEnergy(0, levels[run]);
        anim->init();
        CapturingTestRenderer renderer;
        for (int i = 0; i < 20; i++) {
            resetCapture();
            anim->tick(renderer, 33);
        }
        zassert_equal(barRows(0), kTestHeight, "run %d must reach full height", run);
        audio.setEnergy(0, 0.0f);
        for (int i = 0; i < 6; i++) {
            resetCapture();
            anim->tick(renderer, 33);
            decay[run][i] = barRows(0);
        }
    }
    for (int i = 0; i < 6; i++) {
        zassert_equal(decay[0][i], decay[1][i],
                      "tick %d after the spike: %zu rows vs %zu at the ceiling", i,
                      decay[0][i], decay[1][i]);
    }
    zassert_equal(decay[0][5], 0u, "the bar must have fully decayed after 6 frames");
}

/* Same power in a higher bucket draws a taller bar by the treble tilt, and tilt 0
 * removes the difference. E = 0.03 (−15.2 dB): bucket 0 → 0.58 (2 rows), bucket 3
 * (1.89 octaves × 3 dB) → 0.74 (3 rows). */
ZTEST(fft_bars_animation_di_tests, test_tilt_lifts_high_bucket) {
    MutableAudioSource audio;
    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);

    auto rowsFor = [&](size_t x) {
        audio.resetAll();
        audio.setEnergy(0, 0.03f);
        audio.setEnergy(3, 0.03f);
        anim->init();
        CapturingTestRenderer renderer;
        for (int i = 0; i < 20; i++) {
            resetCapture();
            anim->tick(renderer, 33);
        }
        return barRows(x);
    };

    zassert_equal(rowsFor(0), 2u, "bucket 0 at −15.2 dB: 2 of 4 rows");
    zassert_equal(rowsFor(3), 3u, "bucket 3 gets +5.7 dB of tilt: 3 of 4 rows");

    FakeFftConfig flat;
    flat.tilt = 0.0f;
    anim->setConfigSource(flat);
    zassert_equal(rowsFor(0), rowsFor(3), "tilt 0 renders every bucket alike");
}

/* The legacy energy-scale parameter is a relative gain: ×10 is +10 dB. */
ZTEST(fft_bars_animation_di_tests, test_energy_scale_is_relative_gain) {
    MutableAudioSource audio;
    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);

    FakeFftConfig cfg;
    anim->setConfigSource(cfg);
    zassert_equal(renderRows(anim, audio, 0.03f, 20), 2u, "scale 20 (unity): 2 rows");
    cfg.energyScale = 200.0f;
    zassert_equal(renderRows(anim, audio, 0.03f, 20), 3u, "scale 200 (+10 dB): 3 rows");
    cfg.energyScale = 2.0f;
    zassert_equal(renderRows(anim, audio, 0.03f, 20), 1u, "scale 2 (−10 dB): 1 row");
}

/* No config source (the constexpr fallbacks) must render exactly like a source at the
 * table defaults — the BT-free path and a virgin board agree. */
ZTEST(fft_bars_animation_di_tests, test_fallback_defaults_match_table) {
    MutableAudioSource audio;
    FftBarsAnimation *anim = FftBarsAnimation::getInstance();
    anim->setAudioSource(audio);

    const float probes[] = {1e-4f, 0.003f, 0.03f, 0.3f, 1.0f};
    for (float e : probes) {
        anim->clearConfigSource();
        const size_t fallback = renderRows(anim, audio, e, 12);
        FakeFftConfig cfg; /* table defaults */
        anim->setConfigSource(cfg);
        const size_t viaSource = renderRows(anim, audio, e, 12);
        zassert_equal(fallback, viaSource, "E = %g: fallback %zu rows, config source %zu",
                      (double)e, fallback, viaSource);
    }
}
