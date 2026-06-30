#include <zephyr/ztest.h>

// Expose private members for state inspection
#define private public
#include <animations/glim_player_animation.h>
#undef private

#include <animations/animation_renderer.h>
#include <storage/glim_registry.h>
#include <zephyr/fs/fs.h>
#include <cstring>

extern "C" {
#include <ff.h>
}

namespace {

static constexpr size_t kWidth = 40;
static constexpr size_t kHeight = 12;

struct PixelState {
    uint8_t r = 0, g = 0, b = 0;
    bool isBlack() const { return r == 0 && g == 0 && b == 0; }
};

PixelState sPixels[kWidth][kHeight];

class CapturingRenderer : public AnimationRenderer {
   public:
    size_t displayWidth() const override { return kWidth; }
    size_t displayHeight() const override { return kHeight; }
    void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override {
        if (x < kWidth && y < kHeight) {
            sPixels[x][y] = {r, g, b};
        }
    }
};

void resetPixels() {
    for (size_t x = 0; x < kWidth; x++)
        for (size_t y = 0; y < kHeight; y++) sPixels[x][y] = {};
}

bool anyPixelLit() {
    for (size_t x = 0; x < kWidth; x++)
        for (size_t y = 0; y < kHeight; y++)
            if (!sPixels[x][y].isBlack()) return true;
    return false;
}

bool allPixelsMatch(uint8_t r, uint8_t g, uint8_t b) {
    for (size_t x = 0; x < kWidth; x++)
        for (size_t y = 0; y < kHeight; y++)
            if (sPixels[x][y].r != r || sPixels[x][y].g != g || sPixels[x][y].b != b) return false;
    return true;
}

class FakeSelectionSource : public GlimSelectionSource {
   public:
    size_t index = 0;
    size_t currentIndex() const override { return index; }
    void setSelection(size_t i) override { index = i; }
};

class FakeLoopModeSource : public GlimLoopModeSource {
   public:
    GlimLoopMode mode = GlimLoopMode::LoopOne;
    GlimLoopMode get() const override { return mode; }
};

class FakeButtonSource : public AnimationButtonSource {
   public:
    void press(size_t id) {
        if (id < kMax) pending_[id] = true;
    }
    void update() override {
        for (size_t i = 0; i < kMax; i++) {
            snapshot_[i] = pending_[i];
            pending_[i] = false;
        }
    }
    bool wasPressed(size_t id) const override { return id < kMax && snapshot_[id]; }

   private:
    static constexpr size_t kMax = 5;
    bool pending_[kMax] = {};
    bool snapshot_[kMax] = {};
};

FakeSelectionSource sFakeSelection;
FakeLoopModeSource sFakeLoopMode;
FakeButtonSource sFakeButton;
GlimPlayerAnimationDependencies sFakeDeps(sFakeSelection, sFakeLoopMode);

size_t indexOfName(const char *name) {
    for (size_t i = 0; i < glim_registry::count(); i++) {
        const char *n = glim_registry::name(i);
        if (n && strcmp(n, name) == 0) return i;
    }
    return SIZE_MAX;
}

void writeHeader(struct fs_file_t *f, uint8_t format, uint8_t fps, uint16_t width,
                 uint16_t height, uint32_t frameCount, uint8_t mr, uint8_t mg, uint8_t mb) {
    uint8_t hdr[24] = {
        0x4D, 0x49, 0x4C, 0x47,  // magic (LE: 0x474C494D)
        1, 24,                    // version=1, header_size=24
        format, fps,
        (uint8_t)(width & 0xFF), (uint8_t)(width >> 8),
        (uint8_t)(height & 0xFF), (uint8_t)(height >> 8),
        (uint8_t)(frameCount & 0xFF), (uint8_t)((frameCount >> 8) & 0xFF),
        (uint8_t)((frameCount >> 16) & 0xFF), (uint8_t)((frameCount >> 24) & 0xFF),
        24, 0, 0, 0,  // frame_data_offset=24
        mr, mg, mb,
    };
    zassert_equal(fs_write(f, hdr, 24), (ssize_t)24);
}

/* Writes an RGB24 GLIM with one uniform-color frame per entry in `colors`. */
void writeRgb24Glim(const char *name, const uint8_t (*colors)[3], uint32_t frameCount,
                    uint8_t fps = 24) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", glim_registry::kDirectory, name);

    struct fs_file_t f;
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC), "setup: create %s", path);
    writeHeader(&f, 3 /* Rgb24 */, fps, kWidth, kHeight, frameCount, 0, 0, 0);

    static constexpr size_t kFrameBytes = kWidth * kHeight * 3u;
    uint8_t *frame = (uint8_t *)k_malloc(kFrameBytes);
    zassert_not_null(frame, "k_malloc failed");
    for (uint32_t fi = 0; fi < frameCount; fi++) {
        for (size_t i = 0; i < kFrameBytes; i += 3) {
            frame[i] = colors[fi][0];
            frame[i + 1] = colors[fi][1];
            frame[i + 2] = colors[fi][2];
        }
        zassert_equal(fs_write(&f, frame, kFrameBytes), (ssize_t)kFrameBytes);
    }
    k_free(frame);
    fs_close(&f);
}

/* Writes an RGB24 GLIM smaller than the test renderer's 40x12 display, with one uniform-color
 * frame sized exactly width*height*3 bytes (not kWidth*kHeight*3) - the file's own dimensions
 * are the frame buffer's real stride/extent. */
void writeSmallRgb24Glim(const char *name, uint16_t width, uint16_t height, uint8_t r, uint8_t g,
                         uint8_t b) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", glim_registry::kDirectory, name);

    struct fs_file_t f;
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC), "setup: create %s", path);
    writeHeader(&f, 3 /* Rgb24 */, 24, width, height, 1, 0, 0, 0);

    const size_t frameBytes = static_cast<size_t>(width) * height * 3u;
    uint8_t *frame = (uint8_t *)k_malloc(frameBytes);
    zassert_not_null(frame, "k_malloc failed");
    for (size_t i = 0; i < frameBytes; i += 3) {
        frame[i] = r;
        frame[i + 1] = g;
        frame[i + 2] = b;
    }
    zassert_equal(fs_write(&f, frame, frameBytes), (ssize_t)frameBytes);
    k_free(frame);
    fs_close(&f);
}

/* Writes a mono (Raw) GLIM with a single all-on frame using the given header color. */
void writeMonoGlim(const char *name, uint8_t mr, uint8_t mg, uint8_t mb, uint8_t fps = 24) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", glim_registry::kDirectory, name);

    struct fs_file_t f;
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC), "setup: create %s", path);
    writeHeader(&f, 1 /* Raw */, fps, kWidth, kHeight, 1, mr, mg, mb);

    static constexpr size_t kFrameBytes = (kWidth * kHeight + 7u) / 8u;
    uint8_t frame[kFrameBytes];
    memset(frame, 0xFF, sizeof(frame));
    zassert_equal(fs_write(&f, frame, kFrameBytes), (ssize_t)kFrameBytes);
    fs_close(&f);
}

/* Writes a GLIM whose declared dimensions exceed the test renderer's 40x12 display. */
void writeOversizedGlim(const char *name) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", glim_registry::kDirectory, name);

    struct fs_file_t f;
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC), "setup: create %s", path);
    writeHeader(&f, 3 /* Rgb24 */, 24, 999, 999, 1, 0, 0, 0);
    fs_close(&f);
}

}  // namespace

// ---------------------------------------------------------------------------
// No-filesystem suite — glim_registry is empty, no /NAND: mount exists yet.
// ---------------------------------------------------------------------------

ZTEST_SUITE(glim_player_animation_di, NULL, NULL, NULL, NULL, NULL);

ZTEST(glim_player_animation_di, test_no_dependencies_renders_black) {
    GlimPlayerAnimation *anim = GlimPlayerAnimation::getInstance();

    CapturingRenderer renderer;
    resetPixels();
    anim->tick(renderer, 16);
    zassert_false(anyPixelLit(), "With no dependencies set, tick() must render black");
}

ZTEST(glim_player_animation_di, test_no_files_enters_error_state) {
    GlimPlayerAnimation *anim = GlimPlayerAnimation::getInstance();
    anim->setDependencies(sFakeDeps);
    sFakeSelection.index = 0;
    anim->setActive(true);

    CapturingRenderer renderer;
    resetPixels();
    anim->tick(renderer, 50);
    zassert_true(anim->inErrorState_, "Empty registry must enter error state");
    zassert_true(anyPixelLit(), "Error state must render the 'NO FILE' message");
    anim->setActive(false);
}

// ---------------------------------------------------------------------------
// Happy-path suite — requires the FAT filesystem on the NAND RAM disk. Each test
// reformats the disk first so glim_registry starts from a known-empty directory:
// the registry's discovery state is process-global, so without this, files left
// behind by earlier tests would leak into later ones and make index assumptions
// (e.g. "the 2nd file written gets the next sequential index") unreliable.
// ---------------------------------------------------------------------------

static FATFS s_nand_fat;
static struct fs_mount_t s_nand_mnt = {
    .type = FS_FATFS,
    .mnt_point = "/NAND:",
    .fs_data = &s_nand_fat,
};
static bool s_nand_ready = false;

static void *nand_fs_setup(void) {
    int rc = fs_mkfs(FS_FATFS, (uintptr_t)"NAND", NULL, 0);
    if (rc != 0) {
        return NULL;
    }
    rc = fs_mount(&s_nand_mnt);
    if (rc != 0) {
        return NULL;
    }
    s_nand_ready = true;
    return &s_nand_mnt;
}

static void nand_fs_teardown(void *) {
    if (s_nand_ready) {
        fs_unmount(&s_nand_mnt);
        s_nand_ready = false;
    }
}

static void reset_nand(void) {
    if (s_nand_ready) {
        fs_unmount(&s_nand_mnt);
        s_nand_ready = false;
    }
    zassert_ok(fs_mkfs(FS_FATFS, (uintptr_t)"NAND", NULL, 0));
    zassert_ok(fs_mount(&s_nand_mnt));
    s_nand_ready = true;
    // Tests write files directly into glim_registry::kDirectory before ever calling
    // glim_registry::init() (which would otherwise create it), so create it up front.
    zassert_ok(fs_mkdir(glim_registry::kDirectory));
}

ZTEST_SUITE(glim_player_animation_di_io, NULL, nand_fs_setup, NULL, NULL, nand_fs_teardown);

ZTEST(glim_player_animation_di_io, test_opens_selected_file_and_renders_rgb24) {
    reset_nand();
    const uint8_t colors[2][3] = {{10, 20, 30}, {40, 50, 60}};
    writeRgb24Glim("a.glim", colors, 2, 12);
    glim_registry::init();
    zassert_equal(glim_registry::count(), 1u);

    GlimPlayerAnimation *anim = GlimPlayerAnimation::getInstance();
    anim->setDependencies(sFakeDeps);
    anim->setButtonSource(sFakeButton);
    sFakeSelection.index = 0;
    sFakeLoopMode.mode = GlimLoopMode::LoopOne;
    anim->setActive(true);

    CapturingRenderer renderer;
    resetPixels();
    anim->tick(renderer, 10);  // 12 fps -> 83 ms/frame; 10 ms stays on frame 0
    zassert_false(anim->inErrorState_, "Must not be in error state when file is present");
    zassert_true(anim->decoder_.isOpen());
    zassert_true(allPixelsMatch(10, 20, 30), "All pixels should equal frame 0's color");
    anim->setActive(false);
}

/* A GLIM file declaring dimensions smaller than the display must render only its own pixels
 * in the top-left corner and black everywhere else - not read past the (smaller) frame buffer
 * using the display's width as the stride. */
ZTEST(glim_player_animation_di_io, test_smaller_than_display_renders_only_declared_pixels) {
    reset_nand();
    writeSmallRgb24Glim("a.glim", 4, 4, 10, 20, 30);
    glim_registry::init();
    zassert_equal(glim_registry::count(), 1u);

    GlimPlayerAnimation *anim = GlimPlayerAnimation::getInstance();
    anim->setDependencies(sFakeDeps);
    anim->setButtonSource(sFakeButton);
    sFakeSelection.index = 0;
    sFakeLoopMode.mode = GlimLoopMode::LoopOne;
    anim->setActive(true);

    CapturingRenderer renderer;
    resetPixels();
    anim->tick(renderer, 1);
    zassert_false(anim->inErrorState_, "Smaller-than-display files must be valid, not an error");

    for (size_t x = 0; x < kWidth; x++) {
        for (size_t y = 0; y < kHeight; y++) {
            if (x < 4 && y < 4) {
                zassert_true(sPixels[x][y].r == 10 && sPixels[x][y].g == 20 &&
                                 sPixels[x][y].b == 30,
                             "Pixel (%zu,%zu) within the declared 4x4 frame must match its color",
                             x, y);
            } else {
                zassert_true(sPixels[x][y].isBlack(),
                             "Pixel (%zu,%zu) outside the declared 4x4 frame must be black", x, y);
            }
        }
    }
    anim->setActive(false);
}

ZTEST(glim_player_animation_di_io, test_tick_advances_frame) {
    reset_nand();
    const uint8_t colors[2][3] = {{10, 20, 30}, {40, 50, 60}};
    writeRgb24Glim("a.glim", colors, 2, 12);
    glim_registry::init();

    GlimPlayerAnimation *anim = GlimPlayerAnimation::getInstance();
    anim->setDependencies(sFakeDeps);
    anim->setButtonSource(sFakeButton);
    sFakeSelection.index = 0;
    sFakeLoopMode.mode = GlimLoopMode::LoopOne;
    anim->setActive(true);

    CapturingRenderer renderer;
    anim->tick(renderer, 10);
    zassert_equal(anim->currentFrame_, 0u);

    /* 12 fps -> 83 ms/frame. 90 ms advances exactly one frame. */
    anim->tick(renderer, 90);
    zassert_equal(anim->currentFrame_, 1u);

    resetPixels();
    anim->tick(renderer, 1);
    zassert_true(allPixelsMatch(40, 50, 60), "Frame 1's color should now be rendered");
    anim->setActive(false);
}

ZTEST(glim_player_animation_di_io, test_mono_raw_renders_header_color) {
    reset_nand();
    writeMonoGlim("m.glim", 200, 100, 50, 24);
    glim_registry::init();

    GlimPlayerAnimation *anim = GlimPlayerAnimation::getInstance();
    anim->setDependencies(sFakeDeps);
    anim->setButtonSource(sFakeButton);
    sFakeSelection.index = 0;
    sFakeLoopMode.mode = GlimLoopMode::LoopOne;
    anim->setActive(true);

    CapturingRenderer renderer;
    resetPixels();
    anim->tick(renderer, 10);
    zassert_false(anim->inErrorState_);
    zassert_true(allPixelsMatch(200, 100, 50), "Mono frame should use the header's mono color");
    anim->setActive(false);
}

ZTEST(glim_player_animation_di_io, test_loopone_wraps_same_file) {
    reset_nand();
    const uint8_t colors[1][3] = {{10, 20, 30}};
    writeRgb24Glim("loop_a.glim", colors, 1, 24);  // 41 ms/frame, single frame
    glim_registry::init();
    size_t idx = indexOfName("loop_a.glim");

    GlimPlayerAnimation *anim = GlimPlayerAnimation::getInstance();
    anim->setDependencies(sFakeDeps);
    anim->setButtonSource(sFakeButton);
    sFakeSelection.index = idx;
    sFakeLoopMode.mode = GlimLoopMode::LoopOne;
    anim->setActive(true);

    CapturingRenderer renderer;
    anim->tick(renderer, 10);  // opens file, stays on frame 0
    zassert_equal(anim->currentFrame_, 0u);

    anim->tick(renderer, 50);  // crosses the 41 ms boundary -> clip finishes -> LoopOne wraps
    zassert_equal(anim->currentFrame_, 0u, "LoopOne must wrap back to frame 0");
    zassert_equal(anim->openIndex_, idx, "LoopOne must stay on the same file");
    zassert_false(anim->inErrorState_);
    anim->setActive(false);
}

ZTEST(glim_player_animation_di_io, test_playall_advances_to_next_file) {
    reset_nand();
    const uint8_t colorsA[1][3] = {{1, 2, 3}};
    writeRgb24Glim("playall_a.glim", colorsA, 1, 24);
    writeMonoGlim("playall_b.glim", 9, 8, 7, 24);
    glim_registry::init();
    zassert_equal(glim_registry::count(), 2u);
    size_t idxA = indexOfName("playall_a.glim");
    size_t idxB = indexOfName("playall_b.glim");
    zassert_not_equal(idxA, idxB);

    GlimPlayerAnimation *anim = GlimPlayerAnimation::getInstance();
    anim->setDependencies(sFakeDeps);
    anim->setButtonSource(sFakeButton);
    sFakeSelection.index = idxA;
    sFakeLoopMode.mode = GlimLoopMode::PlayAll;
    anim->setActive(true);

    CapturingRenderer renderer;
    anim->tick(renderer, 10);  // opens A, frame 0
    zassert_equal(anim->openIndex_, idxA);

    anim->tick(renderer, 50);  // A's only frame finishes -> PlayAll advances to B
    zassert_equal(anim->openIndex_, idxB, "PlayAll must advance to the next registry index");
    zassert_equal(sFakeSelection.currentIndex(), idxB,
                 "The selection source must be updated so BLE/app state stays in sync");
    zassert_false(anim->inErrorState_);
    anim->setActive(false);
}

ZTEST(glim_player_animation_di_io, test_stopafterone_freezes_last_frame) {
    reset_nand();
    const uint8_t colors[1][3] = {{77, 88, 99}};
    writeRgb24Glim("stop_a.glim", colors, 1, 24);
    glim_registry::init();
    size_t idx = indexOfName("stop_a.glim");

    GlimPlayerAnimation *anim = GlimPlayerAnimation::getInstance();
    anim->setDependencies(sFakeDeps);
    anim->setButtonSource(sFakeButton);
    sFakeSelection.index = idx;
    sFakeLoopMode.mode = GlimLoopMode::StopAfterOne;
    anim->setActive(true);

    CapturingRenderer renderer;
    anim->tick(renderer, 10);
    anim->tick(renderer, 50);  // clip finishes -> StopAfterOne freezes
    zassert_true(anim->finishedOnce_, "StopAfterOne must latch finishedOnce_");
    zassert_equal(anim->currentFrame_, 0u, "Single-frame clip freezes on its only frame");

    resetPixels();
    anim->tick(renderer, 1000);  // further ticks must not advance or error out
    zassert_true(anim->finishedOnce_);
    zassert_false(anim->inErrorState_);
    zassert_true(allPixelsMatch(77, 88, 99), "Must keep rendering the frozen last frame");
    anim->setActive(false);
}

ZTEST(glim_player_animation_di_io, test_button_press_advances_selection) {
    reset_nand();
    const uint8_t colorsA[1][3] = {{1, 1, 1}};
    const uint8_t colorsB[1][3] = {{2, 2, 2}};
    writeRgb24Glim("btn_a.glim", colorsA, 1, 24);
    writeRgb24Glim("btn_b.glim", colorsB, 1, 24);
    glim_registry::init();
    size_t idxA = indexOfName("btn_a.glim");
    size_t idxB = indexOfName("btn_b.glim");

    GlimPlayerAnimation *anim = GlimPlayerAnimation::getInstance();
    anim->setDependencies(sFakeDeps);
    anim->setButtonSource(sFakeButton);
    sFakeSelection.index = idxA;
    sFakeLoopMode.mode = GlimLoopMode::LoopOne;  // not PlayAll: only the button should advance us
    anim->setActive(true);

    CapturingRenderer renderer;
    anim->tick(renderer, 10);
    zassert_equal(anim->openIndex_, idxA);

    sFakeButton.press(0);
    anim->tick(renderer, 1);  // short tick: well before LoopOne's own clip-end would fire
    zassert_equal(anim->openIndex_, idxB, "Button 0 (Up) must advance to the next registry index");
    zassert_equal(sFakeSelection.currentIndex(), idxB);
    anim->setActive(false);
}

ZTEST(glim_player_animation_di_io, test_button_3_goes_to_previous_file) {
    reset_nand();
    const uint8_t colorsA[1][3] = {{1, 1, 1}};
    const uint8_t colorsB[1][3] = {{2, 2, 2}};
    writeRgb24Glim("btn3_a.glim", colorsA, 1, 24);
    writeRgb24Glim("btn3_b.glim", colorsB, 1, 24);
    glim_registry::init();
    size_t idxA = indexOfName("btn3_a.glim");
    size_t idxB = indexOfName("btn3_b.glim");

    GlimPlayerAnimation *anim = GlimPlayerAnimation::getInstance();
    anim->setDependencies(sFakeDeps);
    anim->setButtonSource(sFakeButton);
    sFakeSelection.index = idxA;
    sFakeLoopMode.mode = GlimLoopMode::LoopOne;
    anim->setActive(true);

    CapturingRenderer renderer;
    anim->tick(renderer, 10);
    zassert_equal(anim->openIndex_, idxA);

    sFakeButton.press(3);
    anim->tick(renderer, 1);
    zassert_equal(anim->openIndex_, idxB, "Button 3 (Down) must go to the previous registry index");
    zassert_equal(sFakeSelection.currentIndex(), idxB);

    // Pressing it again must wrap back around to A (only 2 files in this registry).
    sFakeButton.press(3);
    anim->tick(renderer, 1);
    zassert_equal(anim->openIndex_, idxA, "Previous from the first file must wrap to the last");
    anim->setActive(false);
}

ZTEST(glim_player_animation_di_io, test_external_selection_change_reopens_file) {
    reset_nand();
    const uint8_t colorsA[1][3] = {{1, 1, 1}};
    const uint8_t colorsB[1][3] = {{2, 2, 2}};
    writeRgb24Glim("sel_a.glim", colorsA, 1, 24);
    writeRgb24Glim("sel_b.glim", colorsB, 1, 24);
    glim_registry::init();
    size_t idxA = indexOfName("sel_a.glim");
    size_t idxB = indexOfName("sel_b.glim");

    GlimPlayerAnimation *anim = GlimPlayerAnimation::getInstance();
    anim->setDependencies(sFakeDeps);
    anim->setButtonSource(sFakeButton);
    sFakeSelection.index = idxA;
    sFakeLoopMode.mode = GlimLoopMode::LoopOne;
    anim->setActive(true);

    CapturingRenderer renderer;
    anim->tick(renderer, 10);
    zassert_equal(anim->openIndex_, idxA);

    // Simulate a BLE write landing on the selection source between ticks.
    sFakeSelection.setSelection(idxB);
    anim->tick(renderer, 1);
    zassert_equal(anim->openIndex_, idxB, "tick() must pick up an externally-changed selection");
    anim->setActive(false);
}

ZTEST(glim_player_animation_di_io, test_oversized_dimensions_enters_error_state) {
    reset_nand();
    writeOversizedGlim("big.glim");
    glim_registry::init();
    size_t idx = indexOfName("big.glim");

    GlimPlayerAnimation *anim = GlimPlayerAnimation::getInstance();
    anim->setDependencies(sFakeDeps);
    anim->setButtonSource(sFakeButton);
    sFakeSelection.index = idx;
    sFakeLoopMode.mode = GlimLoopMode::LoopOne;
    anim->setActive(true);

    CapturingRenderer renderer;
    resetPixels();
    anim->tick(renderer, 10);
    zassert_true(anim->inErrorState_,
                "Dimensions exceeding the display must be treated as an open failure");
    anim->setActive(false);
}

ZTEST(glim_player_animation_di_io, test_setactive_false_closes_decoder) {
    reset_nand();
    const uint8_t colors[1][3] = {{5, 5, 5}};
    writeRgb24Glim("close.glim", colors, 1, 24);
    glim_registry::init();

    GlimPlayerAnimation *anim = GlimPlayerAnimation::getInstance();
    anim->setDependencies(sFakeDeps);
    anim->setButtonSource(sFakeButton);
    sFakeSelection.index = 0;
    sFakeLoopMode.mode = GlimLoopMode::LoopOne;
    anim->setActive(true);

    CapturingRenderer renderer;
    anim->tick(renderer, 10);
    zassert_true(anim->decoder_.isOpen(), "Precondition: decoder open after a tick");

    anim->setActive(false);
    zassert_false(anim->decoder_.isOpen(), "Decoder must be closed after setActive(false)");
    zassert_false(anim->inErrorState_, "inErrorState_ must be clear after setActive(false)");
}

/* A file with garbage content (wrong GLIM magic) causes decoder_.open() to fail.
 * The fix: openCurrentFile() must set openIndex_ = index (not kInvalidIndex) on
 * failure, so the animation does not retry on every tick until the selection changes. */
ZTEST(glim_player_animation_di_io, test_corrupt_file_sets_openindex_to_suppress_retry) {
    reset_nand();

    // Write a file whose content is all-zeros — wrong GLIM magic, decoder.open() rejects it.
    char path[64];
    snprintf(path, sizeof(path), "%s/corrupt.glim", glim_registry::kDirectory);
    struct fs_file_t f;
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC),
               "setup: create corrupt.glim");
    uint8_t zeros[32] = {};
    fs_write(&f, zeros, sizeof(zeros));
    fs_close(&f);

    glim_registry::init();
    size_t idx = indexOfName("corrupt.glim");
    zassert_not_equal(idx, SIZE_MAX, "corrupt.glim must be in registry");

    GlimPlayerAnimation *anim = GlimPlayerAnimation::getInstance();
    anim->setDependencies(sFakeDeps);
    anim->setButtonSource(sFakeButton);
    sFakeSelection.index = idx;
    sFakeLoopMode.mode = GlimLoopMode::LoopOne;
    anim->setActive(true);

    CapturingRenderer renderer;
    anim->tick(renderer, 10);

    zassert_true(anim->inErrorState_, "Corrupt file must enter error state");
    // Key assertion: openIndex_ must equal the *requested* index (not kInvalidIndex).
    // If it were kInvalidIndex, every tick would call openCurrentFile() again (spam fix).
    zassert_equal(anim->openIndex_, idx,
                  "openIndex_ must match the requested index to suppress per-frame retries");

    anim->setActive(false);
}
