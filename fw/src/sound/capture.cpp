#include "capture.h"

#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sound.h"
#include "storage/fs_util.h"

LOG_MODULE_REGISTER(capture, LOG_LEVEL_INF);

namespace {

constexpr const char *kDir = "/NAND:";
constexpr const char *kPrefix = "cap_";
constexpr const char *kSuffix = ".wav";

/* Bytes per second of capture: 16 kHz mono 16-bit WAV plus the IMU sidecar at
 * 25 Hz. Used to turn free space into a recordable-seconds figure the app can
 * show, and to clamp a requested limit to what will actually fit. */
constexpr uint32_t kBytesPerSecond = 16000u * 2u + 1400u;

/* Headroom left unclaimed so a capture cannot fill the volume to the point where
 * the filesystem itself struggles — the same instinct as record_wav's own check,
 * which keeps 64 KB back. */
constexpr uint32_t kReserveBytes = 64u * 1024u;

/* A capture nobody stops is the normal field case, so the cap is not optional.
 * This is also close to the physical ceiling: the volume holds roughly three
 * minutes of audio in total. */
constexpr uint32_t kMaxLimitS = 180;

K_SEM_DEFINE(s_start_sem, 0, 1);
K_MUTEX_DEFINE(s_lock);

/* How often the idle worker re-derives the volume-backed numbers (capture count,
 * recordable seconds). This is not cosmetic polling — it is the only thing that
 * makes those two values true:
 *
 *  - The volume mounts at SYS_INIT APPLICATION 90; every capture SYS_INIT hook runs
 *    long before that, so anything sampled at init reads an unmounted filesystem and
 *    lands on 0. Read-only characteristics keep whatever they were last given, so a
 *    boot-time 0 is permanent — the app renders "no room left" on an empty volume
 *    and disables its Record button. Observed on hardware 2026-08-12.
 *  - Files also arrive and disappear over USB mass storage, which the device cannot
 *    observe at all. Even a correct value goes stale the moment someone collects
 *    their captures.
 *
 * It runs on the capture worker rather than a workqueue on purpose: fs_statvfs and
 * the directory walk are flash I/O, which must never run on a cooperative-priority
 * thread (fw/CLAUDE.md). Reads only — no flash-endurance cost. */
constexpr uint32_t kIdleRefreshS = 5;

struct capture_state_t {
    enum capture_state state = CAPTURE_IDLE;
    uint32_t limit_s = 0;
    int64_t started_ms = 0;
    int last_error = 0;
    char path[64] = {};
};

capture_state_t s_cap;
capture_state_observer s_observer = nullptr;

void publish() {
    capture_state_observer observer = s_observer;
    if (observer == nullptr) {
        return;
    }
    struct capture_status status;
    capture_get_status(&status);
    observer(&status);
}

uint64_t free_bytes() {
    struct fs_statvfs vfs;
    if (fs_statvfs(kDir, &vfs) != 0) {
        return 0;
    }
    return (uint64_t)vfs.f_bfree * vfs.f_frsize;
}

/* Highest existing cap_NNNN.wav index, so captures accumulate instead of
 * overwriting each other — the whole point of field capture is coming back with
 * several. Rescanned per capture rather than cached: the volume is also written
 * by a host over USB mass storage, so a cached counter would go stale exactly
 * when someone deletes the ones they already collected. */
uint32_t highest_index() {
    uint32_t highest = 0;
    fs_util::for_each_file(kDir, [&](const char *name) {
        if (strncmp(name, kPrefix, strlen(kPrefix)) != 0) {
            return true;
        }
        if (!fs_util::has_suffix(name, kSuffix)) {
            return true;
        }
        uint32_t index = (uint32_t)strtoul(name + strlen(kPrefix), nullptr, 10);
        if (index > highest) {
            highest = index;
        }
        return true;
    });
    return highest;
}

/* Republish only when a volume-backed number actually moved, so an idle device is
 * not handing the BLE stack an identical status every few seconds. */
void publish_if_changed() {
    static uint32_t last_captures = UINT32_MAX;
    static uint32_t last_remaining_s = UINT32_MAX;

    struct capture_status status;
    capture_get_status(&status);
    if (status.captures == last_captures && status.remaining_s == last_remaining_s) {
        return;
    }
    last_captures = status.captures;
    last_remaining_s = status.remaining_s;

    capture_state_observer observer = s_observer;
    if (observer != nullptr) {
        observer(&status);
    }
}

void worker(void *, void *, void *) {
    for (;;) {
        if (k_sem_take(&s_start_sem, K_SECONDS(kIdleRefreshS)) != 0) {
            publish_if_changed();
            continue;
        }

        uint32_t limit_s;
        char path[sizeof(s_cap.path)];
        k_mutex_lock(&s_lock, K_FOREVER);
        limit_s = s_cap.limit_s;
        memcpy(path, s_cap.path, sizeof(path));
        k_mutex_unlock(&s_lock);

        /* Progress and errors go to the UART shell. A BLE-triggered capture has
         * no shell of its own, and routing to the console keeps one diagnostic
         * path for both front ends rather than a second, log-only one that would
         * drift from it. */
        const struct shell *sh = shell_backend_uart_get_ptr();
        int ret = (sh != nullptr) ? sound_record_wav(sh, limit_s, path) : -ENODEV;

        k_mutex_lock(&s_lock, K_FOREVER);
        s_cap.state = (ret == 0) ? CAPTURE_IDLE : CAPTURE_FAILED;
        s_cap.last_error = ret;
        s_cap.limit_s = 0;
        k_mutex_unlock(&s_lock);

        if (ret == 0) {
            LOG_INF("capture complete: %s", path);
        } else {
            LOG_ERR("capture failed (%d): %s", ret, path);
        }
        publish();
    }
}

K_THREAD_STACK_DEFINE(s_worker_stack, CONFIG_APP_CAPTURE_THREAD_STACK_SIZE);
struct k_thread s_worker_thread;

int capture_init(void) {
    k_thread_create(&s_worker_thread, s_worker_stack, CONFIG_APP_CAPTURE_THREAD_STACK_SIZE, worker,
                    nullptr, nullptr, nullptr, CONFIG_APP_CAPTURE_THREAD_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&s_worker_thread, "capture");
    return 0;
}
SYS_INIT(capture_init, APPLICATION, 2);

}  // namespace

uint32_t capture_remaining_seconds(void) {
    uint64_t avail = free_bytes();
    if (avail <= kReserveBytes) {
        return 0;
    }
    uint64_t seconds = (avail - kReserveBytes) / kBytesPerSecond;
    return (seconds > kMaxLimitS) ? kMaxLimitS : (uint32_t)seconds;
}

uint32_t capture_elapsed_seconds(void) {
    k_mutex_lock(&s_lock, K_FOREVER);
    uint32_t elapsed = (s_cap.state == CAPTURE_RECORDING)
                           ? (uint32_t)((k_uptime_get() - s_cap.started_ms) / 1000)
                           : 0;
    k_mutex_unlock(&s_lock);
    return elapsed;
}

bool capture_is_recording(void) {
    k_mutex_lock(&s_lock, K_FOREVER);
    bool recording = (s_cap.state == CAPTURE_RECORDING);
    k_mutex_unlock(&s_lock);
    return recording;
}

uint32_t capture_count(void) {
    uint32_t count = 0;
    fs_util::for_each_file(kDir, [&](const char *name) {
        if (strncmp(name, kPrefix, strlen(kPrefix)) == 0 && fs_util::has_suffix(name, kSuffix)) {
            count++;
        }
        return true;
    });
    return count;
}

int capture_start(uint32_t limit_s) {
    k_mutex_lock(&s_lock, K_FOREVER);
    if (s_cap.state == CAPTURE_RECORDING) {
        k_mutex_unlock(&s_lock);
        return -EBUSY;
    }
    k_mutex_unlock(&s_lock);

    /* Clamp to what the volume can hold rather than failing: a field user who
     * asks for 60 s on a nearly-full disk is better served by a 20 s capture than
     * by an error they cannot act on without a laptop. */
    uint32_t room_s = capture_remaining_seconds();
    if (room_s == 0) {
        return -ENOSPC;
    }
    if (limit_s == 0 || limit_s > room_s) {
        limit_s = room_s;
    }

    k_mutex_lock(&s_lock, K_FOREVER);
    (void)snprintf(s_cap.path, sizeof(s_cap.path), "%s/%s%04u%s", kDir, kPrefix,
                   (unsigned)(highest_index() + 1), kSuffix);
    s_cap.state = CAPTURE_RECORDING;
    s_cap.limit_s = limit_s;
    s_cap.started_ms = k_uptime_get();
    s_cap.last_error = 0;
    k_mutex_unlock(&s_lock);

    k_sem_give(&s_start_sem);
    publish();
    return 0;
}

int capture_stop(void) {
    k_mutex_lock(&s_lock, K_FOREVER);
    bool running = (s_cap.state == CAPTURE_RECORDING);
    k_mutex_unlock(&s_lock);
    if (!running) {
        return -EALREADY;
    }
    /* The worker finalises the files and publishes the new state itself; this
     * only asks. */
    sound_record_request_stop();
    return 0;
}

void capture_get_status(struct capture_status *out) {
    if (out == nullptr) {
        return;
    }
    k_mutex_lock(&s_lock, K_FOREVER);
    out->state = s_cap.state;
    out->limit_s = s_cap.limit_s;
    out->last_error = s_cap.last_error;
    out->elapsed_s = (s_cap.state == CAPTURE_RECORDING)
                         ? (uint32_t)((k_uptime_get() - s_cap.started_ms) / 1000)
                         : 0;
    k_mutex_unlock(&s_lock);

    out->captures = capture_count();
    out->remaining_s = capture_remaining_seconds();
}

void capture_register_observer(capture_state_observer observer) {
    s_observer = observer;
}

/* Shell front end. The same worker the GATT service drives, so what is exercised
 * here on a bench is what runs in the field — a separate shell-only path would be
 * a second implementation to keep honest. */
static int cmd_capture_start(const struct shell *sh, size_t argc, char **argv) {
    uint32_t limit_s = 0; /* 0 = as long as the volume allows */
    if (argc > 1) {
        limit_s = (uint32_t)strtoul(argv[1], nullptr, 10);
    }
    int ret = capture_start(limit_s);
    if (ret == -EBUSY) {
        shell_error(sh, "a capture is already running");
        return ret;
    }
    if (ret == -ENOSPC) {
        shell_error(sh, "no room left on /NAND: — collect and delete the existing captures");
        return ret;
    }
    if (ret != 0) {
        shell_error(sh, "capture start failed: %d", ret);
        return ret;
    }
    struct capture_status status;
    capture_get_status(&status);
    shell_print(sh, "capture started (limit %u s, %u s of room)", status.limit_s,
                status.remaining_s);
    return 0;
}

static int cmd_capture_stop(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    int ret = capture_stop();
    if (ret == -EALREADY) {
        shell_print(sh, "no capture running");
        return 0;
    }
    shell_print(sh, "stop requested");
    return 0;
}

static int cmd_capture_status(const struct shell *sh, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    struct capture_status status;
    capture_get_status(&status);
    static const char *const kNames[] = {"idle", "recording", "failed"};
    shell_print(sh, "state: %s", kNames[status.state]);
    if (status.state == CAPTURE_RECORDING) {
        shell_print(sh, "elapsed: %u s of %u s", status.elapsed_s, status.limit_s);
    }
    shell_print(sh, "captures on volume: %u", status.captures);
    shell_print(sh, "room remaining: %u s", status.remaining_s);
    if (status.last_error != 0) {
        shell_print(sh, "last error: %d", status.last_error);
    }
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    capture_cmds,
    SHELL_CMD_ARG(start, NULL, "Start a capture: capture start [seconds]", cmd_capture_start, 1, 1),
    SHELL_CMD(stop, NULL, "Stop the running capture early", cmd_capture_stop),
    SHELL_CMD(status, NULL, "Show capture state, count and remaining room", cmd_capture_status),
    SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(capture, &capture_cmds, "On-device audio + IMU capture", NULL);
