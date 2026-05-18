#pragma once

#include <cstddef>

class AnimationAudioSource
{
public:
    virtual ~AnimationAudioSource() = default;

    /* Drain the audio result queue and refresh the internal cache.
     * Call exactly once at the start of each animation tick. */
    virtual void update() = 0;

    /* Beat detection — 4 wide bands. */
    virtual size_t numBands() const = 0;
    virtual float getBandEnergy(size_t band) const = 0;
    virtual bool isBeat(size_t band) const = 0;

    /* Fine-grained display buckets for bar-graph visualisation. */
    virtual size_t numDisplayBuckets() const = 0;
    virtual float getDisplayBucketEnergy(size_t bucket) const = 0;
};
