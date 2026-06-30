#pragma once

class AnimationImuSource {
   public:
    virtual ~AnimationImuSource() = default;

    /* Drain the IMU result queue and refresh the internal cache.
     * Call exactly once at the start of each animation tick. */
    virtual void update() = 0;

    /* Raw accelerometer readings in m/s². */
    virtual float getAccelX() const = 0;
    virtual float getAccelY() const = 0;
    virtual float getAccelZ() const = 0;

    /* Raw gyroscope readings in rad/s. */
    virtual float getGyroX() const = 0;
    virtual float getGyroY() const = 0;
    virtual float getGyroZ() const = 0;
};
