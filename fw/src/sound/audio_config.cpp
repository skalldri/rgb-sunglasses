#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/persistent_characteristic.h>
#include <sound/audio_config.h>
#include <zephyr/logging/log.h>

#include <algorithm>

LOG_MODULE_REGISTER(audio_config, LOG_LEVEL_INF);

constexpr bt_uuid_128 kAudioConfigServiceUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, AudioConfig::kServiceIdNum, 0x56789abc0000));

BtGattPrimaryService<kAudioConfigServiceUuid> audioConfigPrimaryService;

BtGattPersistentCharacteristic<"audio/flux_gamma", "Flux Gamma", true, float, 1000.0f>
    audioFluxGamma;
BtGattPersistentCharacteristic<"audio/beat_flux_floor", "Beat Flux Floor", true, float, 0.005f>
    audioBeatFluxFloor;
/* Default retuned 3.5 -> 0.3 in Phase 3 (issue #264) — derivation in
 * DefaultAudioDspConfigProvider (audio_dsp.cpp) and the PR body. NOTE this only
 * affects boards with no persisted value: an already-provisioned board keeps
 * whatever it stored (the shared dev board carries 1.5 from Phase 1), so
 * on-device verification must set it explicitly via "sound dsp set alpha". */
BtGattPersistentCharacteristic<"audio/beat_alpha", "Beat Alpha", true, float, 0.3f> audioBeatAlpha;
BtGattPersistentCharacteristic<"audio/beat_refractory_frames", "Beat Refractory Frames", true,
                               uint32_t, 5>
    audioBeatRefractoryFrames;
/* Target defaults retuned in Phase 2 (issue #264) from the ABGT 250 baseline
 * captures — derivation in DefaultAgcConfigProvider (sound.cpp) and the PR. */
BtGattPersistentCharacteristic<"audio/agc_target_low", "AGC Target Low", true, float, 0.002f>
    audioAgcTargetLow;
BtGattPersistentCharacteristic<"audio/agc_target_high", "AGC Target High", true, float, 0.05f>
    audioAgcTargetHigh;
BtGattPersistentCharacteristic<"audio/agc_rate_limit_frames", "AGC Rate Limit Frames", true,
                               uint32_t, 10>
    audioAgcRateLimitFrames;
BtGattPersistentCharacteristic<"audio/fft_smoothing_coeff", "FFT Smoothing Coeff", true, float,
                               0.3f>
    audioFftSmoothingCoeff;
BtGattPersistentCharacteristic<"audio/fft_energy_scale", "FFT Energy Scale", true, float, 20.0f>
    audioFftEnergyScale;
/* Phase 2 AGC tunables (issue #264) — appended AFTER the existing providers:
 * BtGattServer assigns UUIDs positionally, so appending preserves every
 * existing characteristic's UUID. */
BtGattPersistentCharacteristic<"audio/agc_attack_frames", "AGC Attack Frames", true, uint32_t, 3>
    audioAgcAttackFrames;
BtGattPersistentCharacteristic<"audio/agc_release_frames", "AGC Release Frames", true, uint32_t,
                               15>
    audioAgcReleaseFrames;
BtGattPersistentCharacteristic<"audio/noise_gate_rms", "AGC Noise Gate RMS", true, float, 0.001f>
    audioNoiseGateRms;
/* Phase 3 threshold-shape tunables (issue #264) — appended AFTER the existing
 * providers for the same positional-UUID reason as the Phase 2 block above.
 *
 * notify=true, matching every sibling in this service. It is load-bearing, not
 * cosmetic: both getters below clamp on read and assign the clamped value back,
 * and that write-back only reaches the app as a notification. The app's write
 * path optimistically shows what it wrote and never re-reads, so with
 * notify=false an out-of-range write (mode=2, sf_delta=5.0) would leave the UI
 * displaying that value indefinitely while the firmware ran the clamped one.
 * The write-back happens on the DSP thread's next getter call (~32 ms), not
 * inside the write handler, so it does not hit the "corrective notify races the
 * write response" hazard documented in fw/CLAUDE.md.
 *
 * CCC cost: two more entries per bonded peer. CONFIG_BT_SETTINGS_CCC_STORE_MAX
 * is 96 (see fw/prj.conf, which records why it was raised from the stock 48
 * during Phase 2) and the limit is PER BOND, not global — Zephyr's ccc_save
 * builds one struct ccc_store[CCC_STORE_MAX] per peer address
 * (zephyr/subsys/bluetooth/host/gatt.c). */
BtGattPersistentCharacteristic<"audio/sf_delta", "Beat SF Delta", true, float, 0.10f> audioSfDelta;
BtGattPersistentCharacteristic<"audio/threshold_mode", "Beat Threshold Mode", true, uint32_t, 0>
    audioThresholdMode;

BtGattServer audioConfigServer(audioConfigPrimaryService, audioFluxGamma, audioBeatFluxFloor,
                               audioBeatAlpha, audioBeatRefractoryFrames, audioAgcTargetLow,
                               audioAgcTargetHigh, audioAgcRateLimitFrames, audioFftSmoothingCoeff,
                               audioFftEnergyScale, audioAgcAttackFrames, audioAgcReleaseFrames,
                               audioNoiseGateRms, audioSfDelta, audioThresholdMode);
BT_GATT_SERVER_REGISTER(audioConfigServerStatic, audioConfigServer);

// Each getter clamps to a sane range and writes the clamped value back, mirroring
// CoreConfig::getBrightnessFactor() (fw/src/core_config.cpp) exactly.

float AudioConfig::getFluxGamma() {
    float value = audioFluxGamma;
    float clamped = std::clamp(value, 1.0f, 100000.0f);
    if (clamped != value) {
        audioFluxGamma = clamped;
    }
    return clamped;
}

void AudioConfig::setFluxGamma(float value) {
    audioFluxGamma = std::clamp(value, 1.0f, 100000.0f);
    // operator= does not invoke onWrite/persistence (see persistent_characteristic.h) -
    // this is a non-BT-write mutation path (shell), so it must mark dirty and request the
    // save itself, mirroring setTargetLow() below.
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioFluxGamma.mark_dirty();
        persistent_value_store::request_save();
    }
}

float AudioConfig::getBeatFluxFloor() {
    float value = audioBeatFluxFloor;
    float clamped = std::clamp(value, 0.0f, 1.0f);
    if (clamped != value) {
        audioBeatFluxFloor = clamped;
    }
    return clamped;
}

void AudioConfig::setBeatFluxFloor(float value) {
    audioBeatFluxFloor = std::clamp(value, 0.0f, 1.0f);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioBeatFluxFloor.mark_dirty();
        persistent_value_store::request_save();
    }
}

float AudioConfig::getBeatAlpha() {
    float value = audioBeatAlpha;
    float clamped = std::clamp(value, 0.1f, 20.0f);
    if (clamped != value) {
        audioBeatAlpha = clamped;
    }
    return clamped;
}

void AudioConfig::setBeatAlpha(float value) {
    audioBeatAlpha = std::clamp(value, 0.1f, 20.0f);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioBeatAlpha.mark_dirty();
        persistent_value_store::request_save();
    }
}

uint32_t AudioConfig::getBeatRefractoryFrames() {
    uint32_t value = audioBeatRefractoryFrames;
    // Clamped to fit the uint8_t per-band refractory counter in audio_dsp.cpp.
    uint32_t clamped = std::clamp<uint32_t>(value, 0, 255);
    if (clamped != value) {
        audioBeatRefractoryFrames = clamped;
    }
    return clamped;
}

void AudioConfig::setBeatRefractoryFrames(uint32_t value) {
    // Clamped to fit the uint8_t per-band refractory counter in audio_dsp.cpp.
    audioBeatRefractoryFrames = std::clamp<uint32_t>(value, 0, 255);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioBeatRefractoryFrames.mark_dirty();
        persistent_value_store::request_save();
    }
}

float AudioConfig::getSfDelta() {
    float value = audioSfDelta;
    float clamped = std::clamp(value, 0.0f, 2.0f);
    if (clamped != value) {
        audioSfDelta = clamped;
    }
    return clamped;
}

void AudioConfig::setSfDelta(float value) {
    audioSfDelta = std::clamp(value, 0.0f, 2.0f);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioSfDelta.mark_dirty();
        persistent_value_store::request_save();
    }
}

uint32_t AudioConfig::getThresholdMode() {
    uint32_t value = audioThresholdMode;
    uint32_t clamped = std::clamp<uint32_t>(value, AUDIO_THRESHOLD_MODE_MEAN_SIGMA,
                                            AUDIO_THRESHOLD_MODE_MEDIAN_DELTA);
    if (clamped != value) {
        audioThresholdMode = clamped;
    }
    return clamped;
}

void AudioConfig::setThresholdMode(uint32_t value) {
    audioThresholdMode = std::clamp<uint32_t>(value, AUDIO_THRESHOLD_MODE_MEAN_SIGMA,
                                              AUDIO_THRESHOLD_MODE_MEDIAN_DELTA);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioThresholdMode.mark_dirty();
        persistent_value_store::request_save();
    }
}

float AudioConfig::getTargetLow() {
    float value = audioAgcTargetLow;
    float clamped = std::clamp(value, 0.001f, 0.1f);
    if (clamped != value) {
        audioAgcTargetLow = clamped;
    }
    return clamped;
}

void AudioConfig::setTargetLow(float value) {
    audioAgcTargetLow = std::clamp(value, 0.001f, 0.1f);
    // operator= does not invoke onWrite/persistence (see persistent_characteristic.h) -
    // this is a non-BT-write mutation path (shell), so it must mark dirty and request the
    // save itself, mirroring ConcreteGlimSelectionSource::setSelection().
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioAgcTargetLow.mark_dirty();
        persistent_value_store::request_save();
    }
}

float AudioConfig::getTargetHigh() {
    float value = audioAgcTargetHigh;
    /* Clamp range changed with Phase 2's SEMANTIC change: targetHigh is now
     * compared against INSTANTANEOUS RMS by the attack path (it used to be
     * smoothed RMS in a symmetric window), so any value below 0.02 is invalid
     * by construction — real music's p99 instantaneous RMS is ~0.014, and a
     * stale persisted Phase-1 value (0.008) would make the attack rule fire on
     * nearly every music frame, ratcheting gain to the -20 dB floor. The
     * raised clamp FLOOR (0.001 → 0.02) is deliberate settings migration:
     * already-tuned boards get their stale value corrected on first read via
     * this getter's write-back, with no boot-ordering hazard. Ceiling widened
     * 0.2 → 0.5 (targetHigh is a comfort band, not the loudness ceiling —
     * that's the near-clip peak path). */
    float clamped = std::clamp(value, 0.02f, 0.5f);
    if (clamped != value) {
        audioAgcTargetHigh = clamped;
    }
    return clamped;
}

void AudioConfig::setTargetHigh(float value) {
    audioAgcTargetHigh = std::clamp(value, 0.02f, 0.5f);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioAgcTargetHigh.mark_dirty();
        persistent_value_store::request_save();
    }
}

uint32_t AudioConfig::getAttackFrames() {
    uint32_t value = audioAgcAttackFrames;
    uint32_t clamped = std::clamp<uint32_t>(value, 1, 20);
    if (clamped != value) {
        audioAgcAttackFrames = clamped;
    }
    return clamped;
}

void AudioConfig::setAttackFrames(uint32_t value) {
    audioAgcAttackFrames = std::clamp<uint32_t>(value, 1, 20);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioAgcAttackFrames.mark_dirty();
        persistent_value_store::request_save();
    }
}

uint32_t AudioConfig::getReleaseFrames() {
    uint32_t value = audioAgcReleaseFrames;
    uint32_t clamped = std::clamp<uint32_t>(value, 1, 100);
    if (clamped != value) {
        audioAgcReleaseFrames = clamped;
    }
    return clamped;
}

void AudioConfig::setReleaseFrames(uint32_t value) {
    audioAgcReleaseFrames = std::clamp<uint32_t>(value, 1, 100);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioAgcReleaseFrames.mark_dirty();
        persistent_value_store::request_save();
    }
}

float AudioConfig::getNoiseGateRms() {
    float value = audioNoiseGateRms;
    float clamped = std::clamp(value, 0.0f, 0.02f);
    if (clamped != value) {
        audioNoiseGateRms = clamped;
    }
    return clamped;
}

void AudioConfig::setNoiseGateRms(float value) {
    audioNoiseGateRms = std::clamp(value, 0.0f, 0.02f);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioNoiseGateRms.mark_dirty();
        persistent_value_store::request_save();
    }
}

uint32_t AudioConfig::getRateLimitFrames() {
    uint32_t value = audioAgcRateLimitFrames;
    uint32_t clamped = std::clamp<uint32_t>(value, 1, 100);
    if (clamped != value) {
        audioAgcRateLimitFrames = clamped;
    }
    return clamped;
}

void AudioConfig::setRateLimitFrames(uint32_t value) {
    audioAgcRateLimitFrames = std::clamp<uint32_t>(value, 1, 100);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioAgcRateLimitFrames.mark_dirty();
        persistent_value_store::request_save();
    }
}

float AudioConfig::getSmoothingCoeff() const {
    float value = audioFftSmoothingCoeff;
    float clamped = std::clamp(value, 0.0f, 1.0f);
    if (clamped != value) {
        audioFftSmoothingCoeff = clamped;
    }
    return clamped;
}

float AudioConfig::getEnergyScale() const {
    float value = audioFftEnergyScale;
    float clamped = std::clamp(value, 0.1f, 1000.0f);
    if (clamped != value) {
        audioFftEnergyScale = clamped;
    }
    return clamped;
}

void audio_dsp_bind_default_bt_dependencies() {
    audio_dsp_set_config_provider(&AudioConfig::getInstance());
    sound_set_agc_config_provider(&AudioConfig::getInstance());
}
