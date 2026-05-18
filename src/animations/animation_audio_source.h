#pragma once

#include <cstddef>

class AnimationAudioSource
{
public:
    virtual ~AnimationAudioSource() = default;

    /* Drain the audio result queue and refresh the internal cache.
     * Call exactly once at the start of each animation tick. */
    virtual void update() = 0;

    virtual float getBandEnergy(size_t band) const = 0;
    virtual bool isBeat(size_t band) const = 0;
    virtual size_t numBands() const = 0;
};
