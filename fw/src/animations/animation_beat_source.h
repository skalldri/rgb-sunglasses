#pragma once

/**
 * @brief Edge/latch-based beat feed for beat-driven animation behaviour.
 *
 * consumeBeat() returns true iff at least one beat was detected since the previous
 * call. The production implementation latches at audio-queue drain time (see
 * src/sound/animation_adapters/audio_animations_sound.cpp), so it is immune to who
 * drains the queue first in a tick.
 *
 * The latch is per-consumer and consume-once, which has a consequence worth knowing
 * before you depend on it: a consumer that stops calling consumeBeat() (an inactive
 * animation, or one whose beat mode is switched off) leaves its slot latched by the
 * next beat, and nothing else drains it. Anything that should start from "no beat
 * seen yet" must therefore discard once on entry rather than assume an empty latch.
 *
 * Lives in its own header rather than color_mode_source.h so consumers that only need
 * the interface don't compile ColorModeSource and its <atomic> dependency — same split
 * as animations/animation_audio_source.h.
 */
class AnimationBeatSource {
   public:
    virtual ~AnimationBeatSource() = default;
    virtual bool consumeBeat() = 0;
};
