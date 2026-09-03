/*
 * BLE live audio telemetry (service 9) — the meters behind the app's Audio Tuning screen.
 *
 * WHY A NEW SERVICE RATHER THAN AN EXTRA CHARACTERISTIC ON SERVICE 2. The audio config
 * service carries a load-bearing comment (fw/src/sound/audio_config.cpp) explaining that
 * every characteristic there is notify=false because it once consumed Android's entire
 * ~15-slot notification budget and silently starved SMP/DFU. Putting a notifier back into
 * that service would invite the next person to add a second one. The lifecycles differ too:
 * config values are persistent settings, this is ephemeral and focus-scoped.
 *
 * EXACTLY ONE notifiable characteristic here, deliberately. The app subscribes only while
 * its tuning screen is focused, the same pattern the capture service uses.
 *
 * THREADING. audio_telemetry_publish() runs on the DSP thread; everything expensive happens
 * here on the system workqueue. bt_gatt_notify() walks connections, allocates a TX buffer
 * and can return -ENOMEM — none of which belongs on a thread with a 2 KB stack, a hard
 * 32 ms deadline, and sole ownership of dmic_read().
 */

#include <bluetooth/bt_conn_activity.h>
#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/persistent_characteristic.h>
#include <string.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "sound/audio_telemetry.h"
#include "sound/audio_param_blob.h"
#include "sound/audio_telemetry_codec.h"
#include "sound/audio_telemetry_control.h"

LOG_MODULE_REGISTER(audio_telemetry_bt, LOG_LEVEL_INF);

/* The notify payload, as a POD the framework can store and compare.
 *
 * Always the full spectrum-tier size in memory; only the bytes the current tier occupies
 * are actually sent, via the BtGattNotifyTraits specialisation below. Sizing the storage to
 * the largest tier keeps the characteristic a fixed-layout type (which the framework's NTTP
 * default requires) while letting the wire length vary per frame. */
struct AudioTelemetryPacked {
    uint8_t bytes[AUDIO_TELEMETRY_SIZE_SPECTRUM];
};

/* operator= on the characteristic compares before notifying, so the type needs this. Kept a
 * free function so AudioTelemetryPacked stays an aggregate and remains usable as an NTTP. */
inline bool operator!=(const AudioTelemetryPacked &a, const AudioTelemetryPacked &b) {
    return memcmp(a.bytes, b.bytes, sizeof(a.bytes)) != 0;
}

template <>
struct BtGattCpfTraits<AudioTelemetryPacked> {
    static constexpr bool kSupported = true;
    static constexpr bt_gatt_cpf kValue = {
        .format = BLE_GATT_CPF_FORMAT_STRUCT,
    };
};

template <>
struct BtGattNotifyTraits<AudioTelemetryPacked> {
    /* Send only the bytes this frame's tier actually uses. The tier is read back out of the
     * packed header rather than from a separate variable, so the length can never disagree
     * with the frame it describes — which is what makes the tiers safely nested. */
    static size_t length(const AudioTelemetryPacked &value) {
        const size_t n = audio_telemetry_tier_size(audio_telemetry_packed_tier(value.bytes));
        return n != 0 ? n : AUDIO_TELEMETRY_SIZE_METERS;
    }
};

/* The codec's ATT-floor constant and the framework's are two independent derivations of the
 * same 20 (MTU 23 minus the 3-byte notify header, issue #115). Neither header can include
 * the other — the codec stays C-compatible and free of BT headers, and the framework's copy
 * is scoped inside a C++ traits struct — but this file sees both, so tie them here. Without
 * it, raising one and not the other silently sizes payloads against a floor that no longer
 * holds, and the failure mode is a dropped notify with no error: exactly what both comments
 * were written about. */
static_assert(AUDIO_TELEMETRY_UNNEGOTIATED_ATT_PAYLOAD ==
                  BtGattNotifyTraits<BtGattDropdownList<32>>::kGuaranteedSafeNotifyLen,
              "the telemetry codec and bt_gatt_traits.h disagree on the unnegotiated ATT floor");

/* Slot 2: the bulk parameter metadata the Audio Tuning screen's sliders need — range,
 * default, step, unit and enum labels for all 17 tunables, in one read.
 *
 * It lives HERE rather than on the audio config service (2) for the same reason the stream
 * does: that service's every characteristic is notify=false because it once exhausted
 * Android's ~15-registration budget, and it is the service the app enumerates parameter by
 * parameter. Adding a ~413-byte blob to it would be read on every discovery of every
 * parameter-bearing screen. Here it is read once, on the tuning screen's focus, by a client
 * that already wants the telemetry service.
 *
 * Read-only and never persisted: it is derived entirely from compile-time constants, so a
 * stored copy could only ever be a stale duplicate of the image's own table. */
namespace {

/* Slot 9: 1 = core config, 2 = audio config, 4 = mcuboot updater, 5 = battery,
 * 6 = power debug, 7 = shuffle, 8 = capture. Cannot collide with
 * BT_ANIMATION_SERVICE_UUID (anim_id << 8, always a multiple of 256) or the extension
 * range (0x40 + slot). */
constexpr bt_uuid_128 kAudioTelemetryServiceUuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 9, 0x56789abc0000));

BtGattPrimaryService<kAudioTelemetryServiceUuid> audioTelemetryPrimaryService;

/* Control word layout — see AUDIO_TELEMETRY_TIER_* in audio_telemetry_codec.h.
 *
 *   [ 7:0]  tier     0 = off, 1 = meters, 2 = +raw stats, 3 = +spectrum
 *   [15:8]  rate_hz  0 => Kconfig default; clamped to [1, kMaxRateHz]
 *   [23:16] hold_s   0 => kDefaultHoldS; clamped to [kMinHoldS, 255] (8-bit field)
 *   [31:24] reserved MUST be zero
 *
 * ORDERING: subscribe to Audio Telemetry BEFORE writing a non-zero tier here. Arming an
 * unsubscribed characteristic is rejected with an ATT error rather than accepted and
 * silently voided.
 */
constexpr uint16_t kMinHoldS = 5;

/* The watchdog is a hard requirement, not polish. A phone that backgrounds, walks out of
 * range, or is simply put in a pocket must not leave the device notifying into a void and
 * holding the radio up until the battery dies. The app re-arms by re-writing control every
 * hold_s/2 while its screen is focused. */
constexpr uint16_t kDefaultHoldS = 60;

class TelemetryControlCharacteristic
    : public BtGattAutoCharacteristicExt<TelemetryControlCharacteristic, "Telemetry Control",
                                         false /* Notify: write-only command */,
                                         false /* ReadOnly */, uint32_t, 0> {
   public:
    using Base = BtGattAutoCharacteristicExt<TelemetryControlCharacteristic, "Telemetry Control",
                                             false, false, uint32_t, 0>;
    using Base::operator=;

    int onWriteChecked(const uint32_t &control);
};

TelemetryControlCharacteristic telemetryControl;

/* The stream itself — the ONE notifiable characteristic in this service. */
BtGattAutoReadNotifyCharacteristic<"Audio Telemetry", AudioTelemetryPacked, AudioTelemetryPacked{}>
    audioTelemetry;

/* Served straight from rodata rather than through a typed characteristic: the typed ones keep
 * a `storage_` copy of the NTTP default, which put the whole compile-time constant into RAM
 * (measured when the blob was 346 B: `datas` grew by 412 B; it is ~413 B now).
 * BtGattRodataBlobCharacteristic costs a pointer
 * and a length instead. Reads still fragment via ATT_READ_BLOB, so it works at MTU 23. */
BtGattRodataBlobCharacteristic<"Audio Param Ranges"> audioParamRanges(kAudioParamBlob.data(),
                                                                     kAudioParamBlobSize);

BtGattServer audioTelemetryServer(audioTelemetryPrimaryService, telemetryControl, audioTelemetry,
                                  audioParamRanges);
BT_GATT_SERVER_REGISTER(audioTelemetryServerStatic, audioTelemetryServer);

/* ── Stream state ────────────────────────────────────────────────────────────
 *
 * Written from TWO contexts, not one: the tick (system workqueue) and onWriteChecked (BT RX
 * workqueue). That is safe here only because both are COOPERATIVE on a uniprocessor and
 * neither blocks inside its critical window — so neither can be preempted mid-update by the
 * other. That argument, not the absence of a lock, is what makes this correct, and it is
 * exactly what a future change would invalidate: a preemptible workqueue, an SMP target, or
 * a blocking call added to either path all break it silently. Add a lock if any of those
 * change.
 *
 * There is deliberately no separate "holding the link" flag. It would have to agree with
 * s_tier at all times, and two flags that must agree eventually disagree — an early return
 * added to onWriteChecked that set one without the other would leave the radio pinned with
 * no stream, or a stream running at SLOW. `s_tier != OFF` IS the condition. */

uint8_t s_tier = AUDIO_TELEMETRY_TIER_OFF;
uint8_t s_rate_hz = CONFIG_APP_AUDIO_TELEMETRY_DEFAULT_RATE_HZ;
int64_t s_hold_until_ms;

/* Smallest ATT MTU across connected links, or 0 if none.
 *
 * The MINIMUM rather than a particular link's: bt_gatt_notify(NULL, ...) fans out to every
 * subscriber, so the frame has to fit the worst of them. Uses the same bt_conn_foreach walk
 * as bleAnyConnEncrypted() in bt_service_cpp.h. */
uint16_t min_conn_mtu() {
    uint16_t mtu = 0;
    bt_conn_foreach(
        BT_CONN_TYPE_LE,
        [](struct bt_conn *conn, void *data) {
            uint16_t *out = static_cast<uint16_t *>(data);
            const uint16_t m = bt_gatt_get_mtu(conn);
            if (*out == 0 || m < *out) {
                *out = m;
            }
        },
        &mtu);
    return mtu;
}

void telemetry_tick(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(telemetry_work, telemetry_tick);

void stop_stream(const char *why) {
    if (s_tier == AUDIO_TELEMETRY_TIER_OFF) {
        return; /* already stopped — and therefore already not holding the link */
    }
    LOG_INF("telemetry stream stopped (%s)", why);
    s_tier = AUDIO_TELEMETRY_TIER_OFF;
    audio_telemetry_set_active(false);
    bt_conn_stream_hold(false, 0);
}

void telemetry_tick(struct k_work *work) {
    ARG_UNUSED(work);

    if (s_tier == AUDIO_TELEMETRY_TIER_OFF) {
        return;
    }

    /* Self-terminating rather than cancelled from elsewhere: a cancel racing a handler that
     * has already started would leave the tick rescheduling itself forever. Every reason to
     * stop is re-checked here, so the stream ends within one tick whichever way it ended —
     * the same shape capture_service.cpp's elapsed tick uses. */
    if (k_uptime_get() >= s_hold_until_ms) {
        stop_stream("hold expired - no app re-arm");
        return;
    }
    if (!audioTelemetry.isSubscribed()) {
        /* The app unsubscribed (screen blurred, backgrounded). Stop promptly rather than
         * waiting out the watchdog — this is what stops the radio being held for a
         * subscriber that no longer exists. An arm-before-subscribe start can no longer
         * reach this path: onWriteChecked rejects it up front. */
        stop_stream("unsubscribed");
        return;
    }

    struct audio_telemetry_frame frame;
    if (!audio_telemetry_take(&frame)) {
        /* The DSP produced nothing since the last tick, so this frame is a repeat. Sending
         * it would cost up to 40 log10f calls, a TX buffer and up to 51 B of airtime to
         * deliver data the app must discard — and the framework's bytes-changed guard
         * cannot suppress it, because take() bumped `dropped` (packed at byte 4), so
         * consecutive stale frames always differ. The app learns about the gap from
         * `dropped` on the NEXT fresh frame, which is strictly more useful.
         *
         * Rare at the 8 Hz default (needs a DSP stall); systematic at the 32 Hz
         * calibration rate, where the 31 ms tick period beats the 32 ms production
         * period by roughly one tick per second. */
        k_work_reschedule(&telemetry_work,
                          K_MSEC(1000u / (s_rate_hz > 0 ? s_rate_hz : 1u)));
        return;
    }

    /* Clamp the tier to what this link can actually carry. bt_gatt_notify() cannot
     * fragment, so an over-length frame is dropped outright — on a stale-GATT link stuck at
     * MTU 23 that would mean silence rather than degraded meters. */
    const uint16_t mtu = min_conn_mtu();
    const size_t att_payload = mtu > 3 ? (size_t)(mtu - 3) : 0;
    const uint8_t tier = audio_telemetry_tier_for_mtu(s_tier, att_payload);

    if (tier == AUDIO_TELEMETRY_TIER_OFF) {
        /* Below even the meters tier there is nothing honest to send. Keep the stream
         * armed — the MTU can still be renegotiated — but say so once. */
        LOG_WRN_ONCE("ATT MTU %u cannot carry even the meters tier; not sending", mtu);
    } else {
        AudioTelemetryPacked packed{};
        if (audio_telemetry_pack(&frame, tier, packed.bytes, sizeof(packed.bytes)) > 0) {
            /* Assignment is what notifies — and it only notifies when the bytes CHANGED.
             * That is the behaviour we want: seq advances every real frame, so a genuine
             * update always differs, while a perfectly repeated frame costs no radio time. */
            audioTelemetry = packed;
        }
    }

    const uint32_t period_ms = 1000u / (s_rate_hz > 0 ? s_rate_hz : 1u);
    k_work_reschedule(&telemetry_work, K_MSEC(period_ms > 0 ? period_ms : 1));
}

}  // namespace

int TelemetryControlCharacteristic::onWriteChecked(const uint32_t &control) {
    /* Decode/default/clamp lives in audio_telemetry_control.h so it can be tested without
     * standing up a BT stack — see the ordering note in the control-word comment above for
     * the one rule this function adds on top. */
    const struct audio_telemetry_control req = audio_telemetry_control_parse(
        control, CONFIG_APP_AUDIO_TELEMETRY_DEFAULT_RATE_HZ, kDefaultHoldS, kMinHoldS,
        CONFIG_APP_AUDIO_TELEMETRY_MAX_HOLD_S);

    if (!req.valid) {
        /* Refusing a write means an ATT error, never "success plus a corrective notify" —
         * the app's optimistic update lands after the response and would paper over it
         * (fw/CLAUDE.md). */
        LOG_WRN("rejecting telemetry control 0x%08x", control);
        return req.error;
    }

    if (req.tier == AUDIO_TELEMETRY_TIER_OFF) {
        stop_stream("app request");
        (void)k_work_cancel_delayable(&telemetry_work);
        return 0;
    }

    if (!audioTelemetry.isSubscribed()) {
        /* SUBSCRIBE BEFORE ARMING. Enforced here so the app finds out synchronously, from
         * its own write, instead of silently getting a stream that never delivers.
         *
         * The alternative was worse than a race: ATT transactions serialize on one bearer,
         * so a central that writes control first CANNOT get its CCCD write in before the
         * first tick runs — the tick's unsubscribe check would then kill an ACKed stream
         * deterministically, and cccCfgChanged re-arms nothing, so the late subscription
         * would land on a dead stream. An app polling the documented hold_s/2 re-arm loop
         * would recover after ~30 s of blank meters; one waiting for a first frame would
         * hang forever with no error to act on. */
        LOG_WRN("telemetry armed before subscribing; rejecting");
        return -EACCES;
    }

    const bool was_off = (s_tier == AUDIO_TELEMETRY_TIER_OFF);
    const uint8_t prev_rate = s_rate_hz;

    s_tier = req.tier;
    s_rate_hz = req.rate_hz;
    s_hold_until_ms = k_uptime_get() + (int64_t)req.hold_s * 1000;

    if (was_off) {
        /* Drop anything left over from a previous session so the first frame the app sees
         * is this session's, not a ghost of the last one. */
        audio_telemetry_reset();
        audio_telemetry_set_active(true);
        LOG_INF("telemetry stream started: tier %u, %u Hz, hold %u s", req.tier, req.rate_hz,
                req.hold_s);
    }

    if (was_off || req.rate_hz != prev_rate) {
        /* Edge-driven, never per frame. Re-asserted on a rate change because the governor
         * picks MEDIUM or FAST from it. `was_off` replaces a separate holding flag: the
         * hold is asserted exactly when the stream starts and released only by
         * stop_stream(), so `s_tier != OFF` already answers "are we holding". */
        bt_conn_stream_hold(true, req.rate_hz);
    }

    k_work_reschedule(&telemetry_work, K_NO_WAIT);
    return 0;
}
