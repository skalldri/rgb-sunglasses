/*
 * BLE control for on-device audio + IMU capture (issue #53 tooling).
 *
 * The point is FIELD capture: start a recording from the phone, do the thing,
 * stop it — then collect the files later over USB mass storage. Nothing here
 * transfers files, so the MCUmgr filesystem fence (which is what stops a bonded
 * peer reading /NAND:/mcuboot.bin) is deliberately untouched.
 *
 * A GATT write runs on the BT RX thread, so Control never does the work itself —
 * it hands off to the capture worker and returns immediately. Blocking BT RX for
 * the length of a recording would stall the entire Bluetooth stack.
 */

#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/persistent_characteristic.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "sound/capture.h"

LOG_MODULE_REGISTER(capture_bt, LOG_LEVEL_INF);

namespace {

/* Slot 8: 5 = battery, 6 = power debug, 7 = shuffle. Cannot collide with
 * BT_ANIMATION_SERVICE_UUID (anim_id << 8, so always a multiple of 256) or with
 * the extension range (0x40 + slot, i.e. 0x4000+). */
constexpr bt_uuid_128 kCaptureServiceUuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 8, 0x56789abc0000));

BtGattPrimaryService<kCaptureServiceUuid> capturePrimaryService;

enum : uint32_t {
    kCommandStop = 0,
    kCommandStart = 1,
};

/* Bespoke rather than the persistence mixin: this one has a fallible side effect,
 * so it needs onWriteChecked. A start can legitimately fail (no space, already
 * running), and the app has to SEE that — per fw/CLAUDE.md, refusing a write means
 * returning an ATT error, never "success plus a corrective notify", because the
 * app's optimistic update lands after the response and would paper over it. */
class CaptureControlCharacteristic
    : public BtGattAutoCharacteristicExt<CaptureControlCharacteristic, "Capture Control",
                                         false /* Notify: write-only command */,
                                         false /* ReadOnly */, uint32_t, kCommandStop> {
   public:
    using Base = BtGattAutoCharacteristicExt<CaptureControlCharacteristic, "Capture Control", false,
                                             false, uint32_t, kCommandStop>;
    using Base::operator=;

    int onWriteChecked(const uint32_t &command);
};

BtGattPersistentCharacteristic<"capture/limit_s", "Capture Limit S", false, uint32_t, 30>
    captureLimitS;
CaptureControlCharacteristic captureControl;

/* Notifiable so the phone sees a capture end without polling — including one that
 * hit its own limit rather than being stopped. The app subscribes only while its
 * capture screen is focused, so this costs nothing against Android's ~15-slot
 * registration budget (see fw/src/core_config.cpp). */
BtGattAutoReadNotifyCharacteristic<"Capture State", uint32_t, CAPTURE_IDLE> captureState;
BtGattAutoReadNotifyCharacteristic<"Capture Elapsed S", uint32_t, 0> captureElapsedS;

/* Read-only status the app shows before starting: how long the volume can still
 * record, and how many captures are already waiting to be collected. Not
 * notifiable — they change when files are added or REMOVED over USB, which the
 * device cannot observe live, so a subscription would promise freshness it
 * cannot deliver. The app re-reads them when its screen gains focus. */
BtGattAutoReadOnlyCharacteristic<"Capture Remaining S", uint32_t, 0> captureRemainingS;
BtGattAutoReadOnlyCharacteristic<"Capture Count", uint32_t, 0> captureCount;

BtGattServer captureServer(capturePrimaryService, captureControl, captureLimitS, captureState,
                           captureElapsedS, captureRemainingS, captureCount);
BT_GATT_SERVER_REGISTER(captureServerStatic, captureServer);

int CaptureControlCharacteristic::onWriteChecked(const uint32_t &command) {
    int ret;
    if (command == kCommandStart) {
        /* 0 means "as long as the volume allows" — the manager clamps it. */
        ret = capture_start(captureLimitS.value());
    } else if (command == kCommandStop) {
        ret = capture_stop();
        if (ret == -EALREADY) {
            /* Stopping an idle capture is a no-op, not a client error: the app
             * may be catching up with a capture that just hit its own limit. */
            ret = 0;
        }
    } else {
        LOG_WRN("unknown capture command %u", command);
        return -EINVAL;
    }

    if (ret != 0) {
        LOG_ERR("capture command %u failed: %d", command, ret);
    }
    return ret;
}

/* The capture manager publishes only on transitions — start, and the end of a
 * recording — because the worker thread is blocked inside the capture loop for
 * everything in between. Without this tick, "Capture Elapsed S" would read 0 for
 * the whole recording, which is worse than not exposing it: an app progress
 * readout would sit frozen and look like a hung capture.
 *
 * capture_elapsed_seconds() is the mutex-only accessor precisely so this can run
 * on the system workqueue — capture_get_status() walks the volume for the file
 * count, and that flash I/O must never happen on a cooperative-priority thread. */
void elapsed_tick(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(elapsed_work, elapsed_tick);

void elapsed_tick(struct k_work *work) {
    ARG_UNUSED(work);
    /* Self-terminating rather than cancelled by the observer: a cancel racing a
     * handler that has already started would leave the tick rescheduling itself
     * forever. Re-checking here means the tick stops within a second of the
     * capture ending, whichever way it ended. */
    if (!capture_is_recording()) {
        return;
    }
    captureElapsedS = capture_elapsed_seconds();
    (void)k_work_schedule(&elapsed_work, K_SECONDS(1));
}

/* Runs on the capture worker, not on a BT thread. operator= notifies, and
 * bt_gatt_notify is safe from any thread. */
void on_capture_state(const struct capture_status *status) {
    captureState = (uint32_t)status->state;
    captureElapsedS = status->elapsed_s;
    captureRemainingS = status->remaining_s;
    captureCount = status->captures;

    if (status->state == CAPTURE_RECORDING) {
        (void)k_work_schedule(&elapsed_work, K_SECONDS(1));
    }
}

int capture_service_init(void) {
    capture_register_observer(on_capture_state);
    /* Seed the read-only values so a phone connecting before the first capture
     * sees real numbers rather than the zero defaults. */
    struct capture_status status;
    capture_get_status(&status);
    on_capture_state(&status);
    return 0;
}
/* After capture_init (APPLICATION 2) so the manager exists to be queried. */
SYS_INIT(capture_service_init, APPLICATION, 3);

}  // namespace
