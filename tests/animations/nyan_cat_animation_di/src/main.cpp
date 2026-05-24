#include <zephyr/ztest.h>

#define private public
#include <animations/nyan_cat_animation.h>
#undef private

#include <animations/animation_renderer.h>
#include <zephyr/fs/fs.h>
#include <cstring>

extern "C" {
#include <ff.h>
}

namespace {

static constexpr size_t kWidth  = 40;
static constexpr size_t kHeight = 12;

struct PixelState {
    uint8_t r = 0, g = 0, b = 0;
    bool isBlack() const { return r == 0 && g == 0 && b == 0; }
};

PixelState sPixels[kWidth][kHeight];

class CapturingRenderer : public AnimationRenderer {
   public:
    size_t displayWidth()  const override { return kWidth; }
    size_t displayHeight() const override { return kHeight; }
    void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override {
        if (x < kWidth && y < kHeight) sPixels[x][y] = {r, g, b};
    }
};

void resetPixels() {
    for (size_t x = 0; x < kWidth; x++)
        for (size_t y = 0; y < kHeight; y++)
            sPixels[x][y] = {};
}

bool anyPixelLit() {
    for (size_t x = 0; x < kWidth; x++)
        for (size_t y = 0; y < kHeight; y++)
            if (!sPixels[x][y].isBlack()) return true;
    return false;
}

bool anyPixelColored() {
    /* Returns true if any lit pixel has non-equal RGB channels (not just white/grey). */
    for (size_t x = 0; x < kWidth; x++)
        for (size_t y = 0; y < kHeight; y++)
            if (!sPixels[x][y].isBlack() &&
                (sPixels[x][y].r != sPixels[x][y].g || sPixels[x][y].g != sPixels[x][y].b))
                return true;
    return false;
}

/* Write a minimal 2-frame 40×12 RGB24 GLIM to path.
 * Frame 0: uniform colour (r0,g0,b0). Frame 1: uniform colour (r1,g1,b1). */
static void write_nyan_cat_glim(const char *path,
                                 uint8_t r0, uint8_t g0, uint8_t b0,
                                 uint8_t r1, uint8_t g1, uint8_t b1)
{
    static constexpr uint32_t kFrameBytes = 40u * 12u * 3u; // 1440
    /* Magic 0x474C494D stored little-endian → bytes {0x4D, 0x49, 0x4C, 0x47} */
    uint8_t hdr[24] = {
        0x4D, 0x49, 0x4C, 0x47,  // magic (LE: 0x474C494D)
        1, 24,                    // version=1, header_size=24
        3, 12,                    // format=Rgb24, fps=12
        40, 0, 12, 0,             // width=40, height=12
        2, 0, 0, 0,               // frame_count=2
        24, 0, 0, 0,              // frame_data_offset=24
        0, 0, 0, 0,               // mono_color unused for Rgb24, reserved
    };

    struct fs_file_t f;
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC),
               "setup: create %s", path);
    zassert_equal(fs_write(&f, hdr, 24), (ssize_t)24);

    uint8_t *frame = (uint8_t *)k_malloc(kFrameBytes);
    zassert_not_null(frame, "k_malloc failed");

    for (uint32_t i = 0; i < kFrameBytes; i += 3) {
        frame[i] = r0; frame[i+1] = g0; frame[i+2] = b0;
    }
    zassert_equal(fs_write(&f, frame, kFrameBytes), (ssize_t)kFrameBytes);

    for (uint32_t i = 0; i < kFrameBytes; i += 3) {
        frame[i] = r1; frame[i+1] = g1; frame[i+2] = b1;
    }
    zassert_equal(fs_write(&f, frame, kFrameBytes), (ssize_t)kFrameBytes);

    k_free(frame);
    fs_close(&f);
}

}  // namespace

// ---------------------------------------------------------------------------
// Error state suite — no NAND filesystem required
// ---------------------------------------------------------------------------

ZTEST_SUITE(nyan_cat_animation_di, NULL, NULL, NULL, NULL, NULL);

/* On native_sim there is no NAND filesystem; both open paths must fail → error state. */
ZTEST(nyan_cat_animation_di, test_setactive_no_file_enters_error_state)
{
    NyanCatAnimation *anim = NyanCatAnimation::getInstance();
    anim->setActive(true);
    zassert_true(anim->inErrorState_, "Should be in error state when both file paths fail");
    anim->setActive(false);
}

/* Error state renders the rainbow "NO FILE" message — at least one pixel lit. */
ZTEST(nyan_cat_animation_di, test_error_state_tick_renders_pixels)
{
    NyanCatAnimation *anim = NyanCatAnimation::getInstance();
    anim->setActive(true);
    zassert_true(anim->inErrorState_, "Precondition: must be in error state");

    CapturingRenderer renderer;
    resetPixels();
    anim->tick(renderer, 50);

    zassert_true(anyPixelLit(), "Error state should render 'NO FILE' with at least one lit pixel");
    anim->setActive(false);
}

/* The rainbow error message uses actual colors (not just white). */
ZTEST(nyan_cat_animation_di, test_error_state_renders_rainbow_colors)
{
    NyanCatAnimation *anim = NyanCatAnimation::getInstance();
    anim->setActive(true);

    CapturingRenderer renderer;
    /* Advance far enough to show multiple characters (each has a different rainbow color). */
    for (int i = 0; i < 10; i++) {
        resetPixels();
        anim->tick(renderer, 50);
    }

    zassert_true(anyPixelColored(),
                 "Rainbow error should produce pixels with non-greyscale colors");
    anim->setActive(false);
}

/* setActive(false) must clear the error state. */
ZTEST(nyan_cat_animation_di, test_setactive_false_clears_error_state)
{
    NyanCatAnimation *anim = NyanCatAnimation::getInstance();
    anim->setActive(true);
    zassert_true(anim->inErrorState_, "Precondition: must be in error state");

    anim->setActive(false);
    zassert_false(anim->inErrorState_, "setActive(false) must clear inErrorState_");
}

// ---------------------------------------------------------------------------
// Happy-path suite — requires FAT filesystem on the NAND RAM disk
// ---------------------------------------------------------------------------

static FATFS s_nand_fat;
static struct fs_mount_t s_nand_mnt = {
    .type      = FS_FATFS,
    .mnt_point = "/NAND:",
    .fs_data   = &s_nand_fat,
};
static bool s_nand_ready = false;

static void *nand_fs_setup(void)
{
    int rc = fs_mkfs(FS_FATFS, (uintptr_t)"NAND", NULL, 0);
    if (rc != 0) { return NULL; }
    rc = fs_mount(&s_nand_mnt);
    if (rc != 0) { return NULL; }
    s_nand_ready = true;
    return &s_nand_mnt;
}

static void nand_fs_teardown(void *)
{
    if (s_nand_ready) {
        fs_unmount(&s_nand_mnt);
        s_nand_ready = false;
    }
}

ZTEST_SUITE(nyan_cat_animation_di_io, NULL, nand_fs_setup, NULL, NULL, nand_fs_teardown);

/* setActive(true) with a valid RGB24 file must NOT enter error state */
ZTEST(nyan_cat_animation_di_io, test_setactive_opens_file_no_error)
{
    write_nyan_cat_glim("/NAND:/nyan_cat.glim", 10, 20, 30, 40, 50, 60);

    NyanCatAnimation *anim = NyanCatAnimation::getInstance();
    anim->setActive(true);
    zassert_false(anim->inErrorState_, "Must not be in error state when file is present");
    zassert_true(anim->decoder_.isOpen(), "Decoder must be open after successful setActive");
    anim->setActive(false);
}

/* tick() reads RGB24 frame data and renders each pixel's exact color */
ZTEST(nyan_cat_animation_di_io, test_tick_renders_frame0_color)
{
    /* Frame 0: uniform orange (255, 100, 0) */
    write_nyan_cat_glim("/NAND:/nyan_cat.glim", 255, 100, 0, 0, 0, 255);

    NyanCatAnimation *anim = NyanCatAnimation::getInstance();
    anim->setActive(true);
    zassert_false(anim->inErrorState_, "Precondition: no error state");

    CapturingRenderer renderer;
    resetPixels();
    /* 12 fps → 83 ms/frame; 10 ms keeps us on frame 0. */
    anim->tick(renderer, 10);

    /* Every pixel should be (255, 100, 0) — the uniform frame 0 color. */
    for (size_t x = 0; x < kWidth; x++) {
        for (size_t y = 0; y < kHeight; y++) {
            zassert_equal(sPixels[x][y].r, 255u, "R at (%d,%d)", x, y);
            zassert_equal(sPixels[x][y].g, 100u, "G at (%d,%d)", x, y);
            zassert_equal(sPixels[x][y].b, 0u,   "B at (%d,%d)", x, y);
        }
    }
    anim->setActive(false);
}

/* tick() renders colored pixels (non-greyscale) from the RGB24 frame */
ZTEST(nyan_cat_animation_di_io, test_tick_renders_colored_pixels)
{
    /* Frame 0: non-greyscale red (255, 0, 0) */
    write_nyan_cat_glim("/NAND:/nyan_cat.glim", 255, 0, 0, 0, 255, 0);

    NyanCatAnimation *anim = NyanCatAnimation::getInstance();
    anim->setActive(true);
    zassert_false(anim->inErrorState_, "Precondition: no error state");

    CapturingRenderer renderer;
    resetPixels();
    anim->tick(renderer, 10);

    zassert_true(anyPixelLit(),     "Frame 0 must produce lit pixels");
    zassert_true(anyPixelColored(), "RGB24 frame must produce colored (non-grey) pixels");
    anim->setActive(false);
}

/* tick() with enough time advances currentFrame_ */
ZTEST(nyan_cat_animation_di_io, test_tick_advances_frame)
{
    write_nyan_cat_glim("/NAND:/nyan_cat.glim", 10, 20, 30, 100, 150, 200);

    NyanCatAnimation *anim = NyanCatAnimation::getInstance();
    anim->setActive(true);
    zassert_false(anim->inErrorState_, "Precondition: no error state");
    zassert_equal(anim->currentFrame_, 0u, "currentFrame_ must start at 0");

    CapturingRenderer renderer;
    /* 12 fps → 1000/12 = 83 ms/frame. Tick 90 ms to advance one frame. */
    anim->tick(renderer, 90);
    zassert_equal(anim->currentFrame_, 1u, "One frame should have elapsed after 90 ms at 12 fps");
    anim->setActive(false);
}

/* After advancing to frame 1 its distinct color is rendered */
ZTEST(nyan_cat_animation_di_io, test_tick_frame1_color)
{
    /* Frame 0: red. Frame 1: blue. */
    write_nyan_cat_glim("/NAND:/nyan_cat.glim", 255, 0, 0, 0, 0, 255);

    NyanCatAnimation *anim = NyanCatAnimation::getInstance();
    anim->setActive(true);
    zassert_false(anim->inErrorState_, "Precondition: no error state");

    CapturingRenderer renderer;
    /* 12 fps → 83 ms/frame. Tick 90 ms to land on frame 1, then render. */
    anim->tick(renderer, 90);
    zassert_equal(anim->currentFrame_, 1u, "Must be on frame 1");

    resetPixels();
    anim->tick(renderer, 10);  // small tick to stay on frame 1
    zassert_equal(sPixels[0][0].r, 0u,   "Frame 1 R should be 0 (blue)");
    zassert_equal(sPixels[0][0].g, 0u,   "Frame 1 G should be 0 (blue)");
    zassert_equal(sPixels[0][0].b, 255u, "Frame 1 B should be 255 (blue)");
    anim->setActive(false);
}

/* Frame wraps back to 0 after exhausting all frames */
ZTEST(nyan_cat_animation_di_io, test_tick_frame_wraps_to_zero)
{
    write_nyan_cat_glim("/NAND:/nyan_cat.glim", 10, 20, 30, 40, 50, 60);

    NyanCatAnimation *anim = NyanCatAnimation::getInstance();
    anim->setActive(true);
    zassert_false(anim->inErrorState_, "Precondition: no error state");

    CapturingRenderer renderer;
    /* 12 fps → 83 ms/frame. 180 ms → 2 frames elapsed (0→1→(2%2=0)). */
    anim->tick(renderer, 180);
    zassert_equal(anim->currentFrame_, 0u, "Frame must wrap to 0 after two 83-ms steps");
    anim->setActive(false);
}

/* setActive(false) closes the decoder and clears error state */
ZTEST(nyan_cat_animation_di_io, test_setactive_false_closes_decoder)
{
    write_nyan_cat_glim("/NAND:/nyan_cat.glim", 10, 20, 30, 40, 50, 60);

    NyanCatAnimation *anim = NyanCatAnimation::getInstance();
    anim->setActive(true);
    zassert_true(anim->decoder_.isOpen(), "Precondition: decoder open after setActive(true)");

    anim->setActive(false);
    zassert_false(anim->decoder_.isOpen(), "Decoder must be closed after setActive(false)");
    zassert_false(anim->inErrorState_, "inErrorState_ must be clear after setActive(false)");
}

/* Parsed header fields match what was written */
ZTEST(nyan_cat_animation_di_io, test_open_header_fields_accessible)
{
    write_nyan_cat_glim("/NAND:/nyan_cat.glim", 10, 20, 30, 40, 50, 60);

    NyanCatAnimation *anim = NyanCatAnimation::getInstance();
    anim->setActive(true);
    zassert_false(anim->inErrorState_);

    const GlimDecoder::Header &h = anim->decoder_.header();
    zassert_equal(h.fps,        12u);
    zassert_equal(h.width,      40u);
    zassert_equal(h.height,     12u);
    zassert_equal(h.frameCount, 2u);
    zassert_equal((uint8_t)h.format, 3u);  // Rgb24
    anim->setActive(false);
}
