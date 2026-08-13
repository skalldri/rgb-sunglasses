#pragma once

#include <zephyr/kernel.h>

struct imu_analysis_result {
    float accel_x; /* m/s² */
    float accel_y;
    float accel_z;
    float gyro_x;  /* rad/s */
    float gyro_y;
    float gyro_z;
    uint32_t seq;
};

/* Published by imu.cpp; consumed by ImuAnimationImuSource::update(). */
extern struct k_msgq imu_result_q;

/**
 * @brief A tapped sample plus the uptime it was produced at.
 *
 * The timestamp is taken HERE, when the IMU thread publishes, rather than when a
 * capture drains the queue. The queue holds 8 samples and the capture drains once
 * per audio frame, so drain-time stamping would fold queue latency into the very
 * timebase the sidecar exists to establish — the one thing that makes the WAV and
 * the IMU track alignable by construction.
 */
struct imu_tap_sample {
    struct imu_analysis_result result;
    uint32_t uptime_ms;
};

/* Second, INDEPENDENT feed of the same samples, for on-device capture.
 *
 * imu_result_q is depth-4, single-consumer and drop-oldest, so a capture draining
 * it would take frames away from whatever is currently rendering — the animation
 * and the recording would each see a subset. Same problem the audio side solved
 * with audio_tap_q, and the same shape of answer.
 *
 * Nothing drains this in normal operation, so it simply fills and then discards.
 * A capture purges it before starting. */
extern struct k_msgq imu_tap_q;
