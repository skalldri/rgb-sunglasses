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

/* Space budget. These MUST match record_wav_capture()'s own pre-flight in
 * sound.cpp byte for byte, because they are two halves of one contract: this
 * turns free space into the recordable-seconds figure the app shows and the
 * clamp capture_start() applies, and the pre-flight then re-checks the clamped
 * length. If this side is the more generous of the two, the clamp advertises a
 * length the pre-flight rejects with -ENOSPC — and since the free-space clamp
 * now binds before kMaxLimitS on any normal volume, that is not an edge case
 * but every capture at the advertised maximum.
 *
 * Both figures are therefore stated at the AUDIO frame rate (31.25 fps), the
 * rate the pre-flight iterates at:
 *   WAV      1024 B/frame  -> 32,000 B/s
 *   IMU rows   56 B/frame  ->  1,750 B/s   (25 Hz of samples, charged per frame)
 *   D-rows    360 B/frame  -> 11,250 B/s
 * and the reserve carries the two sector-sized prologues (the WAV header and
 * the CSV's padded #PARAMS/#IMU block) on top of the 64 KiB of slack, which
 * the pre-flight also counts. */
constexpr uint32_t kBytesPerFrame =
    1024u + (IS_ENABLED(CONFIG_IMU) ? 56u : 0u) +
    (IS_ENABLED(CONFIG_APP_CAPTURE_AUDIO_SIDECAR) ? 360u : 0u);
constexpr uint32_t kBytesPerSecond = kBytesPerFrame * 1000u / 32u;

/* 64 KiB of slack, plus the WAV prologue, plus the CSV header sector. */
constexpr uint32_t kReserveBytes = 64u * 1024u + 4096u + 4096u;

/* A capture nobody stops is the normal field case, so the cap is not optional.
 * No longer the binding limit, though: with the analysis rows the volume holds
 * ~158 s, so the free-space clamp is what actually stops a capture and this is
 * a backstop against a hypothetically larger volume. */
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

/* The two volume-derived numbers, cached because deriving them is flash I/O.
 *
 * This cache is what lets capture_get_status(), capture_count() and
 * capture_remaining_seconds() honour their "safe from any thread" contract: a GATT
 * write handler runs on the BT RX work queue, which is COOPERATIVE (priority −8,
 * fw/docs/threading.md), and fw/CLAUDE.md forbids flash I/O there outright. An
 * earlier version of this file derived both inline, so every "start capture" write
 * from the phone stalled the entire Bluetooth host for an fs_statvfs plus a full
 * directory walk — a walk whose cost grows with each capture collected, which is
 * exactly what this feature is for.
 *
 * Only the worker thread writes these (see refresh_volume_stats), under s_lock. */
uint32_t s_cached_captures = 0;
uint32_t s_cached_remaining_s = 0;

uint64_t free_bytes() {
    struct fs_statvfs vfs;
    if (fs_statvfs(kDir, &vfs) != 0) {
        return 0;
    }
    return (uint64_t)vfs.f_bfree * vfs.f_frsize;
}

/* Recordable seconds the free space can hold. Worker-thread only — flash I/O. */
uint32_t derive_remaining_seconds() {
    uint64_t avail = free_bytes();
    if (avail <= kReserveBytes) {
        return 0;
    }
    uint64_t seconds = (avail - kReserveBytes) / kBytesPerSecond;
    return (seconds > kMaxLimitS) ? kMaxLimitS : (uint32_t)seconds;
}

/* Highest existing cap_NNNN.wav index, so captures accumulate instead of
 * overwriting each other — the whole point of field capture is coming back with
 * several. Rescanned immediately before each capture rather than kept as a
 * counter: the volume is also written by a host over USB mass storage, so a
 * counter would go stale exactly when someone deletes the ones they already
 * collected. Worker-thread only — this is a directory walk. */
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

/* Capture files currently on the volume. Worker-thread only — directory walk. */
uint32_t count_capture_files() {
    uint32_t count = 0;
    fs_util::for_each_file(kDir, [&](const char *name) {
        if (strncmp(name, kPrefix, strlen(kPrefix)) == 0 && fs_util::has_suffix(name, kSuffix)) {
            count++;
        }
        return true;
    });
    return count;
}

void publish() {
    capture_state_observer observer = s_observer;
    if (observer == nullptr) {
        return;
    }
    struct capture_status status;
    capture_get_status(&status);
    observer(&status);
}

/* Re-derive the cached volume numbers. Worker thread only — this is the ONLY
 * place either of them is computed, which is what keeps flash I/O off every
 * other caller's thread. Returns true if either value moved. */
bool refresh_volume_stats() {
    uint32_t captures = count_capture_files();
    uint32_t remaining_s = derive_remaining_seconds();

    k_mutex_lock(&s_lock, K_FOREVER);
    bool changed = (captures != s_cached_captures) || (remaining_s != s_cached_remaining_s);
    s_cached_captures = captures;
    s_cached_remaining_s = remaining_s;
    k_mutex_unlock(&s_lock);
    return changed;
}

/* Republish only when a volume-backed number actually moved, so an idle device is
 * not handing the BLE stack an identical status every few seconds. */
void refresh_and_publish_if_changed() {
    if (refresh_volume_stats()) {
        publish();
    }
}

void worker(void *, void *, void *) {
    for (;;) {
        if (k_sem_take(&s_start_sem, K_SECONDS(kIdleRefreshS)) != 0) {
            refresh_and_publish_if_changed();
            continue;
        }

        /* Everything the volume has to answer happens HERE, on this thread: the
         * free-space clamp and the next free index. capture_start() only records
         * the request, because it runs on whatever thread asked — for a BLE start
         * that is the cooperative BT RX work queue. */
        refresh_volume_stats();
        uint32_t room_s = capture_remaining_seconds();

        uint32_t limit_s;
        char path[sizeof(s_cap.path)];
        k_mutex_lock(&s_lock, K_FOREVER);
        limit_s = s_cap.limit_s;
        k_mutex_unlock(&s_lock);

        int ret;
        if (room_s == 0) {
            /* The volume filled between the request and now (a host can add files
             * over USB at any moment), so the pre-flight check capture_start() did
             * against the cache no longer holds. */
            ret = -ENOSPC;
            path[0] = '\0';
        } else {
            /* Clamp to what will actually fit rather than failing: a field user who
             * asks for 60 s on a nearly-full disk is better served by a 20 s capture
             * than by an error they cannot act on without a laptop. */
            if (limit_s == 0 || limit_s > room_s) {
                limit_s = room_s;
            }
            (void)snprintf(path, sizeof(path), "%s/%s%04u%s", kDir, kPrefix,
                           (unsigned)(highest_index() + 1), kSuffix);

            /* Publish the clamped limit and the real path before recording, so a
             * progress readout counts against the length that will actually run. */
            k_mutex_lock(&s_lock, K_FOREVER);
            s_cap.limit_s = limit_s;
            memcpy(s_cap.path, path, sizeof(s_cap.path));
            k_mutex_unlock(&s_lock);
            publish();

            /* Progress and errors go to the UART shell. A BLE-triggered capture has
             * no shell of its own, and routing to the console keeps one diagnostic
             * path for both front ends rather than a second, log-only one that would
             * drift from it. */
            const struct shell *sh = shell_backend_uart_get_ptr();
            ret = (sh != nullptr) ? sound_record_wav(sh, limit_s, path) : -ENODEV;
        }

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
        /* The new file changed both volume numbers; refresh before publishing so
         * the app's count and remaining room settle in the same notification. */
        refresh_volume_stats();
        publish();
    }
}

/* K_KERNEL_THREAD_DEFINE, not K_THREAD_STACK_DEFINE + k_thread_create: this thread
 * stays kernel-mode, so it needs none of the dynamic-creation workaround a K_USER
 * conversion does (fw/CLAUDE.md, CONFIG_USERSPACE), and a kernel stack avoids the
 * ~1 KB of privileged stack a userspace-capable stack reserves per thread. */
K_KERNEL_THREAD_DEFINE(capture_worker_thread, CONFIG_APP_CAPTURE_THREAD_STACK_SIZE, worker, NULL,
                       NULL, NULL, CONFIG_APP_CAPTURE_THREAD_PRIORITY, 0, 0);

}  // namespace

uint32_t capture_remaining_seconds(void) {
    k_mutex_lock(&s_lock, K_FOREVER);
    uint32_t remaining_s = s_cached_remaining_s;
    k_mutex_unlock(&s_lock);
    return remaining_s;
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
    k_mutex_lock(&s_lock, K_FOREVER);
    uint32_t count = s_cached_captures;
    k_mutex_unlock(&s_lock);
    return count;
}

int capture_start(uint32_t limit_s) {
    /* Records the request and returns; the worker does the space clamp, the
     * directory scan for the next index, and the recording itself. Nothing here
     * may touch the filesystem: a BLE start runs this on the BT RX work queue,
     * which is cooperative (see the s_cached_* comment above). */
    k_mutex_lock(&s_lock, K_FOREVER);
    if (s_cap.state == CAPTURE_RECORDING) {
        k_mutex_unlock(&s_lock);
        return -EBUSY;
    }
    /* Pre-flight against the cached figure so a start onto a full volume is
     * refused with an ATT error the app can show, rather than accepted and then
     * failed asynchronously. At most kIdleRefreshS stale, and the worker re-checks
     * authoritatively before recording — this only has to catch the case the user
     * can already see on screen. */
    if (s_cached_remaining_s == 0) {
        k_mutex_unlock(&s_lock);
        return -ENOSPC;
    }
    /* Armed at REQUEST time, not when the worker reaches the loop: the state
     * below makes the app's Stop button live immediately, and the worker still
     * has a free-space check and a directory scan to do first. A stop landing in
     * that window has to survive. */
    sound_record_arm();
    /* The path is filled in by the worker once it knows the next free index; until
     * then it is empty rather than stale from the previous capture. */
    s_cap.path[0] = '\0';
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
    /* One lock, no filesystem: every field is either live state or a cached
     * volume figure, which is what makes this callable from the BT RX thread. */
    k_mutex_lock(&s_lock, K_FOREVER);
    out->state = s_cap.state;
    out->limit_s = s_cap.limit_s;
    out->last_error = s_cap.last_error;
    out->elapsed_s = (s_cap.state == CAPTURE_RECORDING)
                         ? (uint32_t)((k_uptime_get() - s_cap.started_ms) / 1000)
                         : 0;
    out->captures = s_cached_captures;
    out->remaining_s = s_cached_remaining_s;
    k_mutex_unlock(&s_lock);
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
