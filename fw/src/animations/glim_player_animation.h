#pragma once

#include <animations/animation.h>
#include <animations/animation_button_source.h>
#include <storage/glim_decoder.h>
#include <led_config.h>
#include <cstdint>

// How the player behaves once the currently-open GLIM finishes its last frame.
enum class GlimLoopMode {
    LoopOne,       // Replay the same file from frame 0.
    PlayAll,       // Advance to the next file in the registry (wrapping), then loop forever.
    StopAfterOne,  // Freeze on the last frame.
};

// Read-side: which registry index is currently selected. Write-side: change the selection
// (from a BLE write or a button press) — both converge on this one call, which is responsible
// for updating whatever backing characteristic notifies the app.
class GlimSelectionSource {
   public:
    virtual ~GlimSelectionSource() = default;
    virtual size_t currentIndex() const = 0;
    virtual void setSelection(size_t index) = 0;
};

class GlimLoopModeSource {
   public:
    virtual ~GlimLoopModeSource() = default;
    virtual GlimLoopMode get() const = 0;
};

class GlimPlayerAnimationDependencies {
   public:
    GlimPlayerAnimationDependencies(GlimSelectionSource &selectionSource,
                                    const GlimLoopModeSource &loopModeSource)
        : selectionSource(selectionSource), loopModeSource(loopModeSource) {}

    GlimSelectionSource &selectionSource;
    const GlimLoopModeSource &loopModeSource;
};

// Generic GLIM player: enumerates /NAND:/glim via glim_registry and can play any file found
// there, picked via BLE (GlimSelectionSource) or a button press (AnimationButtonSource).
// Replaces the old per-file BadAppleAnimation/NyanCatAnimation; the open file's own header
// (not the animation type) determines pixel format (Raw mono vs Rgb24).
class GlimPlayerAnimation : public BaseAnimationTemplate<GlimPlayerAnimation, Animation::GlimPlayer> {
   public:
    void setDependencies(const GlimPlayerAnimationDependencies &deps);

    /* Injected by src/buttons/animation_adapters/button_animation_source.cpp */
    void setButtonSource(AnimationButtonSource &source);

    void init() override;
    void tick(AnimationRenderer &renderer, size_t timeSinceLastTickMs) override;
    void setActive(bool active) override;

   private:
    // Physical button layout is a directional grid (see fw/CLAUDE.md): 0=Up, 1=Left, 2=Right,
    // 3=Down. Up cycles to the next GLIM file (mirroring PlayAll's auto-advance); Down cycles to
    // the previous one. Left/Right are intentionally unassigned.
    static constexpr size_t kNextButtonId = 0;  // Up
    static constexpr size_t kPrevButtonId = 3;  // Down

    static constexpr size_t kInvalidIndex = static_cast<size_t>(-1);

    // Upper bound across both hardware targets (40x12 frame display, RGB24 worst case).
    static constexpr size_t kMaxFrameBytes = kFrameDisplayWidth * kFrameDisplayHeight * 3u;

    // readFrame() decompresses LZ4 frames into frameBuf_ and rejects, at open(), any LZ4 file
    // whose frame exceeds GlimDecoder::kMaxLz4FrameBytes. Both derive from 40x12x3 but live in
    // different files (led_config.h vs glim_decoder.h); tie them together so a display-geometry
    // change here can't silently make the decoder reject valid full-size LZ4 frames.
    static_assert(GlimDecoder::kMaxLz4FrameBytes >= kMaxFrameBytes,
                  "GlimDecoder's LZ4 frame bound is smaller than the player's frame buffer");

    // 1 pixel per 50 ms, matching TextAnimation's default scroll rate.
    static constexpr uint32_t kErrorScrollStepMs = 50u;

    const GlimPlayerAnimationDependencies *deps_ = nullptr;
    AnimationButtonSource *buttonSource_ = nullptr;

    GlimDecoder decoder_;
    size_t openIndex_ = kInvalidIndex;
    uint32_t currentFrame_ = 0;
    // Which frame index currently sits in frameBuf_ (-1 = none loaded). tick() runs faster than
    // the clip's fps, so without this it would re-read+re-decode the same frame several times per
    // displayed frame — cheap for Raw/Rgb24, but a wasted ~1.4 KB LZ4 decompress per render tick
    // for Lz4PerFrameRgb24. Gating readFrame() on advance decodes once per displayed frame.
    int64_t loadedFrame_ = -1;
    uint32_t accumulatedMs_ = 0;
    uint8_t frameBuf_[kMaxFrameBytes];

    bool inErrorState_ = false;
    bool finishedOnce_ = false;  // Latched once StopAfterOne reaches the last frame.

    int32_t errorScrollOffset_ = 0;
    uint32_t errorScrollAccumMs_ = 0;

    void openCurrentFile(size_t index);
    void onClipFinished();
    void renderError(AnimationRenderer &renderer, size_t timeSinceLastTickMs);
};

void glim_player_animation_bind_default_button_dependencies();
void glim_player_animation_bind_default_bt_dependencies();
