#include "imu.h"

#include <zephyr/drivers/sensor.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(imu);

K_MSGQ_DEFINE(imu_result_q, sizeof(struct imu_analysis_result), 4, 4);
K_SEM_DEFINE(imu_drdy_sem, 0, 1);

static const struct device* s_imu_dev;
static uint32_t s_seq;

static void drdy_handler(const struct device* dev, const struct sensor_trigger* trig) {
    ARG_UNUSED(dev);
    ARG_UNUSED(trig);
    k_sem_give(&imu_drdy_sem);
}

static void imu_thread_func(void* a, void* b, void* c) {
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    struct sensor_value odr;
    odr.val1 = 25;

    // Start the accel and gyro at 25Hz
    sensor_attr_set(s_imu_dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
    sensor_attr_set(s_imu_dev, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);

    while (true) {
        /* Block until the BMI270 data-ready interrupt fires. */
        k_sem_take(&imu_drdy_sem, K_FOREVER);

        if (!s_imu_dev) {
            continue;
        }

        int ret = sensor_sample_fetch(s_imu_dev);
        if (ret) {
            LOG_ERR("sensor_sample_fetch failed: %d", ret);
            continue;
        }

        struct sensor_value accel[3];
        struct sensor_value gyro[3];

        ret = sensor_channel_get(s_imu_dev, SENSOR_CHAN_ACCEL_XYZ, accel);
        if (ret) {
            LOG_ERR("accel channel_get failed: %d", ret);
            continue;
        }

        ret = sensor_channel_get(s_imu_dev, SENSOR_CHAN_GYRO_XYZ, gyro);
        if (ret) {
            LOG_ERR("gyro channel_get failed: %d", ret);
            continue;
        }

        struct imu_analysis_result result = {
            .accel_x = sensor_value_to_float(&accel[0]),
            .accel_y = sensor_value_to_float(&accel[1]),
            .accel_z = sensor_value_to_float(&accel[2]),
            .gyro_x = sensor_value_to_float(&gyro[0]),
            .gyro_y = sensor_value_to_float(&gyro[1]),
            .gyro_z = sensor_value_to_float(&gyro[2]),
            .seq = s_seq++,
        };

        /* Discard old frames if the queue is full — keep only the freshest. */
        if (k_msgq_put(&imu_result_q, &result, K_NO_WAIT) == -ENOMSG) {
            k_msgq_purge(&imu_result_q);
            k_msgq_put(&imu_result_q, &result, K_NO_WAIT);
        }
    }
}

K_THREAD_DEFINE(imu_thread, CONFIG_IMU_THREAD_STACK_SIZE, imu_thread_func, NULL, NULL, NULL,
                CONFIG_IMU_THREAD_PRIORITY, K_FP_REGS, 0);

static struct sensor_trigger s_drdy_trig = {
    .type = SENSOR_TRIG_DATA_READY,
    .chan = SENSOR_CHAN_ALL,
};

static int imu_init(void) {
    s_imu_dev = DEVICE_DT_GET(DT_NODELABEL(bmi270));
    if (!device_is_ready(s_imu_dev)) {
        LOG_ERR("BMI270 device not ready");
        s_imu_dev = NULL;
        return -ENODEV;
    }

    int ret = sensor_trigger_set(s_imu_dev, &s_drdy_trig, drdy_handler);
    if (ret) {
        LOG_ERR("sensor_trigger_set failed: %d", ret);
    }
    return ret;
}

SYS_INIT(imu_init, APPLICATION, 2);
