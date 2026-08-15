#pragma once

#include <cstddef>
#include <cstdint>

class AnimationAudioSource {
   public:
    virtual ~AnimationAudioSource() = default;

    /* Drain the audio result queue and refresh the internal cache.
     * Call exactly once at the start of each animation tick. */
    virtual void update() = 0;

    /* Total analysis frames drained since boot (monotonic, wraps at 2^32).
     * Lets consumers advance per NEW frame rather than per render tick, so
     * their response is independent of the render tick rate (issue #376). */
    virtual uint32_t frameCount() const = 0;

    /* Beat detection — 4 wide bands. */
    virtual size_t numBands() const = 0;
    virtual float getBandEnergy(size_t band) const = 0;
    virtual bool isBeat(size_t band) const = 0;

    /* Fine-grained display buckets for bar-graph visualisation. */
    virtual size_t numDisplayBuckets() const = 0;
    virtual float getDisplayBucketEnergy(size_t bucket) const = 0;
};
