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
#include "sound/audio_telemetry_codec.h"

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
 *   [23:16] hold_s   0 => kDefaultHoldS; clamped to [kMinHoldS, Kconfig max]
 *   [31:24] reserved MUST be zero
 */
constexpr uint32_t kTierMask = 0x000000FFu;
constexpr uint32_t kRateShift = 8;
constexpr uint32_t kHoldShift = 16;
constexpr uint32_t kReservedMask = 0xFF000000u;

constexpr uint8_t kMaxRateHz = 32; /* the analysis rate itself — asking for more is a lie */
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

BtGattServer audioTelemetryServer(audioTelemetryPrimaryService, telemetryControl, audioTelemetry);
BT_GATT_SERVER_REGISTER(audioTelemetryServerStatic, audioTelemetryServer);

/* ── Stream state, owned by the workqueue ────────────────────────────────── */

uint8_t s_tier = AUDIO_TELEMETRY_TIER_OFF;
uint8_t s_rate_hz = CONFIG_APP_AUDIO_TELEMETRY_DEFAULT_RATE_HZ;
int64_t s_hold_until_ms;
bool s_holding_link;

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

void release_link_hold() {
    if (s_holding_link) {
        bt_conn_stream_hold(false, 0);
        s_holding_link = false;
    }
}

void stop_stream(const char *why) {
    if (s_tier == AUDIO_TELEMETRY_TIER_OFF) {
        return;
    }
    LOG_INF("telemetry stream stopped (%s)", why);
    s_tier = AUDIO_TELEMETRY_TIER_OFF;
    audio_telemetry_set_active(false);
    release_link_hold();
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
        stop_stream("unsubscribed");
        return;
    }

    struct audio_telemetry_frame frame;
    const bool fresh = audio_telemetry_take(&frame);
    ARG_UNUSED(fresh); /* a stale repeat is still sent; `dropped` is how the app sees it */

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
    if ((control & kReservedMask) != 0) {
        /* Refuse rather than ignore: reserved bits are how a future field gets added, and
         * silently accepting them now would make that field unusable. Per fw/CLAUDE.md,
         * refusing a write means an ATT error — never "success plus a corrective notify",
         * which the app's optimistic update would paper over. */
        LOG_WRN("telemetry control 0x%08x sets reserved bits", control);
        return -EINVAL;
    }

    const uint8_t tier = (uint8_t)(control & kTierMask);
    if (tier > AUDIO_TELEMETRY_TIER_MAX) {
        LOG_WRN("unknown telemetry tier %u", tier);
        return -EINVAL;
    }

    if (tier == AUDIO_TELEMETRY_TIER_OFF) {
        stop_stream("app request");
        (void)k_work_cancel_delayable(&telemetry_work);
        return 0;
    }

    uint8_t rate = (uint8_t)((control >> kRateShift) & 0xFF);
    if (rate == 0) {
        rate = CONFIG_APP_AUDIO_TELEMETRY_DEFAULT_RATE_HZ;
    }
    if (rate > kMaxRateHz) {
        rate = kMaxRateHz;
    }

    uint16_t hold_s = (uint16_t)((control >> kHoldShift) & 0xFF);
    if (hold_s == 0) {
        hold_s = kDefaultHoldS;
    }
    if (hold_s < kMinHoldS) {
        hold_s = kMinHoldS;
    }
    if (hold_s > CONFIG_APP_AUDIO_TELEMETRY_MAX_HOLD_S) {
        hold_s = CONFIG_APP_AUDIO_TELEMETRY_MAX_HOLD_S;
    }

    const bool was_off = (s_tier == AUDIO_TELEMETRY_TIER_OFF);
    const uint8_t prev_rate = s_rate_hz;

    s_tier = tier;
    s_rate_hz = rate;
    s_hold_until_ms = k_uptime_get() + (int64_t)hold_s * 1000;

    if (was_off) {
        /* Drop anything left over from a previous session so the first frame the app sees
         * is this session's, not a ghost of the last one. */
        audio_telemetry_reset();
        audio_telemetry_set_active(true);
        LOG_INF("telemetry stream started: tier %u, %u Hz, hold %u s", tier, rate, hold_s);
    }

    if (!s_holding_link || rate != prev_rate) {
        /* Edge-driven, never per frame. Re-asserted on a rate change because the governor
         * picks MEDIUM or FAST from the rate. */
        bt_conn_stream_hold(true, rate);
        s_holding_link = true;
    }

    k_work_reschedule(&telemetry_work, K_NO_WAIT);
    return 0;
}
