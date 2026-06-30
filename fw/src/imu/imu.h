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
