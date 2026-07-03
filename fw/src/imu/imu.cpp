#include "imu.h"

#include <zephyr/drivers/sensor.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifdef CONFIG_USERSPACE
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/sys/libc-hooks.h>
#endif

LOG_MODULE_REGISTER(imu);

K_MSGQ_DEFINE(imu_result_q, sizeof(struct imu_analysis_result), 4, 4);
K_SEM_DEFINE(imu_drdy_sem, 0, 1);

// imu_thread runs in user mode under CONFIG_USERSPACE (issue #79, proto0 only -- see
// fw/boards/rgb_sunglasses_proto0_nrf5340_cpuapp.conf). s_imu_dev and s_seq are the
// only two plain globals it touches directly (everything else is a syscall-covered
// kernel object it's granted access to in imu_init() below), so they're the only state
// that needs to live in its granted memory partition. struct k_mem_domain is an
// incomplete type without CONFIG_USERSPACE (see app_memory/mem_domain.h), so the
// domain itself -- unlike the globals, which are fine either way since
// K_APPMEM_PARTITION_DEFINE/K_APP_BMEM expand to nothing when disabled -- needs an
// explicit #ifdef.
#ifdef CONFIG_USERSPACE
K_APPMEM_PARTITION_DEFINE(imu_partition);
K_APP_BMEM(imu_partition) static const struct device* s_imu_dev;
K_APP_BMEM(imu_partition) static uint32_t s_seq;
static struct k_mem_domain s_imu_domain;
#else
static const struct device* s_imu_dev;
static uint32_t s_seq;
#endif

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

// A plain stack + struct k_thread rather than K_THREAD_DEFINE (which would statically
// auto-start the thread at K_NO_WAIT) -- see the comment in imu_init() below for why
// this thread must be created dynamically instead.
K_THREAD_STACK_DEFINE(imu_stack, CONFIG_IMU_THREAD_STACK_SIZE);
static struct k_thread s_imu_thread_data;

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
        return ret;
    }

    // Created with K_FOREVER so it doesn't start until the permission/domain setup
    // below (when CONFIG_USERSPACE is on) is complete -- mirrors Zephyr's own
    // recommended pattern for taking a thread to user mode (see
    // samples/userspace/prod_consumer/src/app_a.c's writeback_thread). This has to be
    // a dynamically-created thread rather than a K_THREAD_DEFINE static one: this SoC
    // config has CONFIG_ARCH_HAS_CUSTOM_SWAP_TO_MAIN=1, which means K_THREAD_DEFINE
    // threads are set up with _current == NULL, skipping z_mem_domain_init_thread()
    // and leaving them never linked into any memory domain (confirmed via GDB+SWD --
    // every K_THREAD_DEFINE thread in this project has a zeroed mem_domain_info).
    // k_mem_domain_add_thread() unconditionally tries to unlink the thread from its
    // prior domain first, which crashes on that never-linked list node. Creating the
    // thread here instead means _current is imu_init()'s own (already domain-linked)
    // caller, so the new thread properly inherits a valid domain membership that
    // k_mem_domain_add_thread() can then safely move.
    //
    // This also fixes a latent pre-existing bug: the old K_THREAD_DEFINE thread could
    // start running (and call sensor_attr_set() using s_imu_dev) before this SYS_INIT
    // hook had assigned s_imu_dev at all. Starting the thread explicitly here, after
    // s_imu_dev is set and the trigger is registered, removes that race entirely.
    k_tid_t tid = k_thread_create(&s_imu_thread_data, imu_stack, K_THREAD_STACK_SIZEOF(imu_stack),
                                  imu_thread_func, NULL, NULL, NULL, CONFIG_IMU_THREAD_PRIORITY,
                                  K_FP_REGS | K_USER, K_FOREVER);
    k_thread_name_set(tid, "imu_thread");

#ifdef CONFIG_USERSPACE
    // Grant imu_thread access to the kernel objects it uses directly (sensor device
    // handle, result queue, DRDY semaphore) and add it to a memory domain covering the
    // plain globals above -- user threads have zero permissions by default on
    // anything they don't own.
    k_thread_access_grant(tid, s_imu_dev, &imu_result_q, &imu_drdy_sem);

    // z_libc_partition holds z_arm_tls_ptr (the current thread's TLS pointer, read by
    // every thread at entry via __aeabi_read_tp() when CONFIG_CURRENT_THREAD_USE_TLS
    // is on -- true here) along with libc/stack-canary globals. It's normally part of
    // k_mem_domain_default, so every thread has it by default; moving imu_thread out
    // to its own domain drops that access unless re-added explicitly here (confirmed
    // via GDB+SWD: without this, imu_thread usage-faults on its very first
    // instruction in z_thread_entry(), before imu_thread_func() even starts). Mirrors
    // Zephyr's own samples/userspace/prod_consumer/src/app_a.c, which does the same.
    struct k_mem_partition* parts[] = {&imu_partition, &z_libc_partition};
    ret = k_mem_domain_init(&s_imu_domain, ARRAY_SIZE(parts), parts);
    if (ret) {
        LOG_ERR("k_mem_domain_init failed: %d", ret);
        return ret;
    }

    ret = k_mem_domain_add_thread(&s_imu_domain, tid);
    if (ret) {
        LOG_ERR("k_mem_domain_add_thread failed: %d", ret);
        return ret;
    }
#endif

    k_thread_start(tid);

    return 0;
}

SYS_INIT(imu_init, APPLICATION, 2);
