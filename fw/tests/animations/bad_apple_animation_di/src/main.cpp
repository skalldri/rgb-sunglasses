#include <zephyr/ztest.h>

// Expose private members for state inspection
#define private public
#include <animations/bad_apple_animation.h>
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
        if (x < kWidth && y < kHeight) {
            sPixels[x][y] = {r, g, b};
        }
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

bool allPixelsMatch(uint8_t r, uint8_t g, uint8_t b) {
    for (size_t x = 0; x < kWidth; x++)
        for (size_t y = 0; y < kHeight; y++)
            if (sPixels[x][y].r != r || sPixels[x][y].g != g || sPixels[x][y].b != b)
                return false;
    return true;
}

/* Write a minimal 2-frame 40×12 Raw GLIM to path.
 * Frame 0: all 0xFF (every pixel on). Frame 1: all 0x00 (every pixel off). */
static void write_bad_apple_glim(const char *path,
                                  uint8_t mr, uint8_t mg, uint8_t mb)
{
    static constexpr size_t kFrameBytes = 60u; // ceil(40*12/8)
    /* Magic 0x474C494D stored little-endian → bytes {0x4D, 0x49, 0x4C, 0x47} */
    uint8_t hdr[24] = {
        0x4D, 0x49, 0x4C, 0x47,  // magic (LE: 0x474C494D)
        1, 24,                    // version=1, header_size=24
        1, 24,                    // format=Raw, fps=24
        40, 0, 12, 0,             // width=40, height=12
        2, 0, 0, 0,               // frame_count=2
        24, 0, 0, 0,              // frame_data_offset=24
        mr, mg, mb, 0,            // mono_color, reserved
    };

    struct fs_file_t f;
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC),
               "setup: create %s", path);
    zassert_equal(fs_write(&f, hdr, 24), (ssize_t)24);

    uint8_t frame[kFrameBytes];
    memset(frame, 0xFF, kFrameBytes);
    zassert_equal(fs_write(&f, frame, kFrameBytes), (ssize_t)kFrameBytes);
    memset(frame, 0x00, kFrameBytes);
    zassert_equal(fs_write(&f, frame, kFrameBytes), (ssize_t)kFrameBytes);

    fs_close(&f);
}

}  // namespace

// ---------------------------------------------------------------------------
// Error state suite — no NAND filesystem required
// ---------------------------------------------------------------------------

ZTEST_SUITE(bad_apple_animation_di, NULL, NULL, NULL, NULL, NULL);

/* On native_sim there is no NAND filesystem, so both open paths fail.
 * setActive(true) must enter the error state. */
ZTEST(bad_apple_animation_di, test_setactive_no_file_enters_error_state)
{
    BadAppleAnimation *anim = BadAppleAnimation::getInstance();
    anim->setActive(true);
    zassert_true(anim->inErrorState_, "Should be in error state when both file paths fail");
    anim->setActive(false);
}

/* In error state, tick() must render the scrolling "NO FILE" message —
 * at least one pixel should be lit at the initial scroll position. */
ZTEST(bad_apple_animation_di, test_error_state_tick_renders_pixels)
{
    BadAppleAnimation *anim = BadAppleAnimation::getInstance();
    anim->setActive(true);
    zassert_true(anim->inErrorState_, "Precondition: must be in error state");

    CapturingRenderer renderer;
    resetPixels();
    /* Pass enough time to fully advance one scroll step so text is on-screen. */
    anim->tick(renderer, 50);

    zassert_true(anyPixelLit(), "Error state should render 'NO FILE' with at least one lit pixel");
    anim->setActive(false);
}

/* setActive(false) must clear the error state regardless of how it was entered. */
ZTEST(bad_apple_animation_di, test_setactive_false_clears_error_state)
{
    BadAppleAnimation *anim = BadAppleAnimation::getInstance();
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

ZTEST_SUITE(bad_apple_animation_di_io, NULL, nand_fs_setup, NULL, NULL, nand_fs_teardown);

/* setActive(true) with a valid file present must NOT enter error state */
ZTEST(bad_apple_animation_di_io, test_setactive_opens_file_no_error)
{
    write_bad_apple_glim("/NAND:/bad_apple.glim", 255, 255, 255);

    BadAppleAnimation *anim = BadAppleAnimation::getInstance();
    anim->setActive(true);
    zassert_false(anim->inErrorState_, "Must not be in error state when file is present");
    zassert_true(anim->decoder_.isOpen(), "Decoder must be open after successful setActive");
    anim->setActive(false);
}

/* tick() with frame 0 (all-on) renders all pixels in the mono color from the header */
ZTEST(bad_apple_animation_di_io, test_tick_renders_all_on_frame)
{
    /* mono color = (255, 128, 0) so we can distinguish it from defaults */
    write_bad_apple_glim("/NAND:/bad_apple.glim", 255, 128, 0);

    BadAppleAnimation *anim = BadAppleAnimation::getInstance();
    anim->setActive(true);
    zassert_false(anim->inErrorState_, "Precondition: no error state");

    CapturingRenderer renderer;
    resetPixels();
    /* 24 fps → 41 ms/frame; 10 ms keeps us on frame 0 (all-on). */
    anim->tick(renderer, 10);

    zassert_true(anyPixelLit(), "Frame 0 (all-on) must have at least one lit pixel");
    zassert_true(allPixelsMatch(255, 128, 0),
                 "All pixels should equal the mono color declared in the header");
    anim->setActive(false);
}

/* tick() with enough elapsed time advances currentFrame_ by one */
ZTEST(bad_apple_animation_di_io, test_tick_advances_frame)
{
    write_bad_apple_glim("/NAND:/bad_apple.glim", 255, 255, 255);

    BadAppleAnimation *anim = BadAppleAnimation::getInstance();
    anim->setActive(true);
    zassert_false(anim->inErrorState_, "Precondition: no error state");
    zassert_equal(anim->currentFrame_, 0u, "currentFrame_ must start at 0");

    CapturingRenderer renderer;
    /* 24 fps → 1000/24 = 41 ms per frame. Tick 50 ms to advance exactly one frame. */
    anim->tick(renderer, 50);
    zassert_equal(anim->currentFrame_, 1u, "One frame should have elapsed after 50 ms at 24 fps");
    anim->setActive(false);
}

/* Frame 1 (all-off) renders as all-black */
ZTEST(bad_apple_animation_di_io, test_tick_all_off_frame_renders_black)
{
    write_bad_apple_glim("/NAND:/bad_apple.glim", 255, 0, 0);

    BadAppleAnimation *anim = BadAppleAnimation::getInstance();
    anim->setActive(true);
    zassert_false(anim->inErrorState_, "Precondition: no error state");

    CapturingRenderer renderer;
    /* Advance to frame 1 with a 50 ms tick at 24 fps */
    anim->tick(renderer, 50);
    zassert_equal(anim->currentFrame_, 1u, "Should be on frame 1 after 50 ms");

    resetPixels();
    /* Small tick to stay on frame 1 (all-off) and capture its render */
    anim->tick(renderer, 10);
    zassert_false(anyPixelLit(), "Frame 1 (all-off) should render all-black");
    anim->setActive(false);
}

/* Frame wraps back to 0 after all frames are consumed */
ZTEST(bad_apple_animation_di_io, test_tick_frame_wraps_to_zero)
{
    write_bad_apple_glim("/NAND:/bad_apple.glim", 255, 255, 255);

    BadAppleAnimation *anim = BadAppleAnimation::getInstance();
    anim->setActive(true);
    zassert_false(anim->inErrorState_, "Precondition: no error state");

    CapturingRenderer renderer;
    /* 24 fps → 41 ms/frame. 100 ms → 2 frames elapsed (100/41 = 2 r 18).
     * With 2 frames in the file, frame index wraps: 0→1→(2%2=0). */
    anim->tick(renderer, 100);
    zassert_equal(anim->currentFrame_, 0u, "Frame must wrap back to 0 after two 41-ms steps");
    anim->setActive(false);
}

/* setActive(false) closes the decoder */
ZTEST(bad_apple_animation_di_io, test_setactive_false_closes_decoder)
{
    write_bad_apple_glim("/NAND:/bad_apple.glim", 255, 255, 255);

    BadAppleAnimation *anim = BadAppleAnimation::getInstance();
    anim->setActive(true);
    zassert_true(anim->decoder_.isOpen(), "Precondition: decoder open after setActive(true)");

    anim->setActive(false);
    zassert_false(anim->decoder_.isOpen(), "Decoder must be closed after setActive(false)");
    zassert_false(anim->inErrorState_, "inErrorState_ must be clear after setActive(false)");
}

/* header fields read back from the animation after a successful open */
ZTEST(bad_apple_animation_di_io, test_open_header_fields_accessible)
{
    write_bad_apple_glim("/NAND:/bad_apple.glim", 200, 100, 50);

    BadAppleAnimation *anim = BadAppleAnimation::getInstance();
    anim->setActive(true);
    zassert_false(anim->inErrorState_);

    const GlimDecoder::Header &h = anim->decoder_.header();
    zassert_equal(h.fps,        24u);
    zassert_equal(h.width,      40u);
    zassert_equal(h.height,     12u);
    zassert_equal(h.frameCount, 2u);
    zassert_equal(h.monoColorR, 200u);
    zassert_equal(h.monoColorG, 100u);
    zassert_equal(h.monoColorB, 50u);
    anim->setActive(false);
}
