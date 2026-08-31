#include <bluetooth/bt_service_cpp.h>
#include <bluetooth/persistent_characteristic.h>
#include <sound/audio_config.h>
#include <sound/audio_param_table.h>
#include <zephyr/logging/log.h>

#include <tuple>
#include <utility>

LOG_MODULE_REGISTER(audio_config, LOG_LEVEL_INF);

constexpr bt_uuid_128 kAudioConfigServiceUuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, AudioConfig::kServiceIdNum, 0x56789abc0000));

BtGattPrimaryService<kAudioConfigServiceUuid> audioConfigPrimaryService;

/* Notify=false on every characteristic in this service (it used to be true):
 * Android caps GATT notification registrations at ~15 per app
 * (BTA_GATTC_NOTIF_REG_MAX) and this service alone consumed 15 slots, silently
 * starving the SMP/DFU characteristic's registration (all app firmware-update
 * calls timed out). These values are app-written tunables; the clamp
 * write-back that notify used to carry (getters clamp on read and assign the
 * clamped value back) now reaches the app via its read-back-after-write on
 * non-notifiable characteristics (app/context/bluetooth-context.tsx). */
BtGattPersistentCharacteristic<"audio/flux_gamma", "Flux Gamma", false, float,
                               audioParamDefaultF<kAudioParamFluxGamma>()>
    audioFluxGamma;
/* Default retuned 0.005 -> 0.08 (issue #264, post-Phase-3) — derivation in
 * DefaultAudioDspConfigProvider (audio_dsp.cpp). Provisioned boards keep their
 * persisted value; set it explicitly with "sound dsp set floor" when testing. */
BtGattPersistentCharacteristic<"audio/beat_flux_floor", "Beat Flux Floor", false, float,
                               audioParamDefaultF<kAudioParamBeatFluxFloor>()>
    audioBeatFluxFloor;
/* Default retuned 3.5 -> 0.3 in Phase 3 (issue #264) — derivation in
 * DefaultAudioDspConfigProvider (audio_dsp.cpp) and the PR body. NOTE this only
 * affects boards with no persisted value: an already-provisioned board keeps
 * whatever it stored, so
 * on-device verification must set it explicitly via "sound dsp set alpha".
 * (Measured 2026-08-24 on the shared proto0: it reports 0.3000, i.e. the retuned
 * default. This comment used to assert the board still carried 1.5 from Phase 1 —
 * that was stale, so check with "sound dsp params" rather than assuming either.) */
BtGattPersistentCharacteristic<"audio/beat_alpha", "Beat Alpha", false, float,
                               audioParamDefaultF<kAudioParamBeatAlpha>()>
    audioBeatAlpha;
BtGattPersistentCharacteristic<"audio/beat_refractory_frames", "Beat Refractory Frames", false,
                               uint32_t, audioParamDefaultU<kAudioParamBeatRefractoryFrames>()>
    audioBeatRefractoryFrames;
/* Target defaults retuned in Phase 2 (issue #264) from the ABGT 250 baseline
 * captures — derivation in DefaultAgcConfigProvider (sound.cpp) and the PR. */
BtGattPersistentCharacteristic<"audio/agc_target_low", "AGC Target Low", false, float,
                               audioParamDefaultF<kAudioParamAgcTargetLow>()>
    audioAgcTargetLow;
BtGattPersistentCharacteristic<"audio/agc_target_high", "AGC Target High", false, float,
                               audioParamDefaultF<kAudioParamAgcTargetHigh>()>
    audioAgcTargetHigh;
BtGattPersistentCharacteristic<"audio/agc_rate_limit_frames", "AGC Rate Limit Frames", false,
                               uint32_t, audioParamDefaultU<kAudioParamAgcRateLimitFrames>()>
    audioAgcRateLimitFrames;
BtGattPersistentCharacteristic<"audio/fft_smoothing_coeff", "FFT Smoothing Coeff", false, float,
                               audioParamDefaultF<kAudioParamFftSmoothingCoeff>()>
    audioFftSmoothingCoeff;
BtGattPersistentCharacteristic<"audio/fft_energy_scale", "FFT Energy Scale", false, float,
                               audioParamDefaultF<kAudioParamFftEnergyScale>()>
    audioFftEnergyScale;
/* Phase 2 AGC tunables (issue #264) — appended AFTER the existing providers:
 * BtGattServer assigns UUIDs positionally, so appending preserves every
 * existing characteristic's UUID. */
BtGattPersistentCharacteristic<"audio/agc_attack_frames", "AGC Attack Frames", false, uint32_t,
                               audioParamDefaultU<kAudioParamAgcAttackFrames>()>
    audioAgcAttackFrames;
BtGattPersistentCharacteristic<"audio/agc_release_frames", "AGC Release Frames", false, uint32_t,
                               audioParamDefaultU<kAudioParamAgcReleaseFrames>()>
    audioAgcReleaseFrames;
/* Default retuned 0.001 -> 0.0006 (issue #264, post-Phase-3) — derivation in
 * DefaultAgcConfigProvider (sound.cpp). Same persisted-value caveat as above. */
BtGattPersistentCharacteristic<"audio/noise_gate_rms", "AGC Noise Gate RMS", false, float,
                               audioParamDefaultF<kAudioParamNoiseGateRms>()>
    audioNoiseGateRms;
/* Phase 3 threshold-shape tunables (issue #264) — appended AFTER the existing
 * providers for the same positional-UUID reason as the Phase 2 block above.
 *
 * notify=false, matching every sibling in this service (see the header comment
 * at the top of the declarations). The clamp write-back these getters perform
 * (assigning the clamped value back on read, on the DSP thread's next getter
 * call ~32 ms after an out-of-range write like mode=2 or sf_delta=5.0) reaches
 * the app through its read-back-after-write on non-notifiable characteristics
 * instead of through a notification. */
BtGattPersistentCharacteristic<"audio/sf_delta", "Beat SF Delta", false, float,
                               audioParamDefaultF<kAudioParamSfDelta>()>
    audioSfDelta;
BtGattPersistentCharacteristic<"audio/threshold_mode", "Beat Threshold Mode", false, uint32_t,
                               audioParamDefaultU<kAudioParamThresholdMode>()>
    audioThresholdMode;

BtGattServer audioConfigServer(audioConfigPrimaryService, audioFluxGamma, audioBeatFluxFloor,
                               audioBeatAlpha, audioBeatRefractoryFrames, audioAgcTargetLow,
                               audioAgcTargetHigh, audioAgcRateLimitFrames, audioFftSmoothingCoeff,
                               audioFftEnergyScale, audioAgcAttackFrames, audioAgcReleaseFrames,
                               audioNoiseGateRms, audioSfDelta, audioThresholdMode);
BT_GATT_SERVER_REGISTER(audioConfigServerStatic, audioConfigServer);

/* The settings key and CUD label at each declaration above must be spelled as string
 * literals (they are StringLiteral NTTPs, so they cannot be generated from the table).
 * These assertions stop them drifting away from audio_param_table.h silently — a mismatched
 * key would move a parameter's NVS storage without moving its metadata, and a mismatched
 * label would mislabel it in the companion app.
 *
 * DECLARATION ORDER is separately load-bearing: BtGattServer assigns characteristic UUIDs
 * positionally, so the Nth argument to the BtGattServer(...) call above must be the Nth entry
 * in kAudioParams, and transposing two of those arguments would silently renumber every
 * characteristic after them — breaking the cached handle->meaning mapping on every bonded
 * phone. The per-name assertions below do NOT catch that (each variable's own key and label
 * still match its own index after a transposition), so the argument order is pinned separately
 * by audioServerOrderMatches<> further down. */
static_assert(audioParamStrEq(kAudioParams[kAudioParamFluxGamma].key, "audio/flux_gamma"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamFluxGamma].label, "Flux Gamma"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamBeatFluxFloor].key, "audio/beat_flux_floor"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamBeatFluxFloor].label, "Beat Flux Floor"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamBeatAlpha].key, "audio/beat_alpha"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamBeatAlpha].label, "Beat Alpha"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamBeatRefractoryFrames].key,
                              "audio/beat_refractory_frames"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamBeatRefractoryFrames].label,
                              "Beat Refractory Frames"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamAgcTargetLow].key, "audio/agc_target_low"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamAgcTargetLow].label, "AGC Target Low"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamAgcTargetHigh].key, "audio/agc_target_high"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamAgcTargetHigh].label, "AGC Target High"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamAgcRateLimitFrames].key,
                              "audio/agc_rate_limit_frames"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamAgcRateLimitFrames].label,
                              "AGC Rate Limit Frames"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamFftSmoothingCoeff].key,
                              "audio/fft_smoothing_coeff"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamFftSmoothingCoeff].label,
                              "FFT Smoothing Coeff"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamFftEnergyScale].key,
                              "audio/fft_energy_scale"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamFftEnergyScale].label, "FFT Energy Scale"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamAgcAttackFrames].key,
                              "audio/agc_attack_frames"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamAgcAttackFrames].label, "AGC Attack Frames"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamAgcReleaseFrames].key,
                              "audio/agc_release_frames"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamAgcReleaseFrames].label,
                              "AGC Release Frames"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamNoiseGateRms].key, "audio/noise_gate_rms"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamNoiseGateRms].label, "AGC Noise Gate RMS"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamSfDelta].key, "audio/sf_delta"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamSfDelta].label, "Beat SF Delta"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamThresholdMode].key, "audio/threshold_mode"));
static_assert(audioParamStrEq(kAudioParams[kAudioParamThresholdMode].label, "Beat Threshold Mode"));

/* The table stores the threshold-mode bounds as plain numbers so it stays free of project
 * headers; this is where they are tied back to the real enum. */
static_assert(static_cast<uint32_t>(kAudioParams[kAudioParamThresholdMode].min) ==
                  AUDIO_THRESHOLD_MODE_MEAN_SIGMA,
              "threshold mode table min has drifted from AUDIO_THRESHOLD_MODE_MEAN_SIGMA");
static_assert(static_cast<uint32_t>(kAudioParams[kAudioParamThresholdMode].max) ==
                  AUDIO_THRESHOLD_MODE_MEDIAN_DELTA,
              "threshold mode table max has drifted from AUDIO_THRESHOLD_MODE_MEDIAN_DELTA");

/* The *Frames parameters are only meaningful against the analysis frame period. */
static_assert(kAudioParamFrameMs == BLOCK_CAPTURE_TIME_MS,
              "audio_param_table.h frame period has drifted from sound.h");

/* Pins the BtGattServer(...) ARGUMENT ORDER to kAudioParams, which the per-name assertions
 * above cannot do.
 *
 * BtGattServer is variadic over its providers and every characteristic type carries a static
 * constexpr getDescription() (bt_service_cpp.h), so the Nth provider's description can be
 * compared against the Nth table label at compile time. Provider 0 is the primary-service
 * declaration, hence the +1: characteristic index I is provider index I + 1.
 *
 * Transposing two arguments in the constructor call changes which type lands at a given
 * provider index, so its description stops matching and this fails to build. That is the whole
 * point — the alternative is discovering it as a field bug on already-bonded phones. */
template <size_t I, typename Server>
struct AudioServerProviderAt;

template <size_t I, BtGattAttributeProvider... Providers>
struct AudioServerProviderAt<I, BtGattServer<Providers...>> {
    using type = std::tuple_element_t<I, std::tuple<Providers...>>;
};

template <size_t I>
constexpr bool audioServerOrderMatches() {
    using Provider = typename AudioServerProviderAt<I + 1, decltype(audioConfigServer)>::type;
    return audioParamStrEq(Provider::getDescription(), kAudioParams[I].label);
}

template <size_t... Is>
constexpr bool audioServerOrderMatchesAll(std::index_sequence<Is...>) {
    return (audioServerOrderMatches<Is>() && ...);
}

static_assert(audioServerOrderMatchesAll(std::make_index_sequence<kAudioParamCount>{}),
              "BtGattServer argument order has drifted from kAudioParams order — this would "
              "renumber characteristic UUIDs and break every bonded app");

// Each getter clamps to a sane range and writes the clamped value back, mirroring
// CoreConfig::getBrightnessFactor() (fw/src/core_config.cpp) exactly.

float AudioConfig::getFluxGamma() {
    float value = audioFluxGamma;
    float clamped = audioParamClampF<kAudioParamFluxGamma>(value);
    if (clamped != value) {
        audioFluxGamma = clamped;
    }
    return clamped;
}

void AudioConfig::setFluxGamma(float value) {
    audioFluxGamma = audioParamClampF<kAudioParamFluxGamma>(value);
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
    float clamped = audioParamClampF<kAudioParamBeatFluxFloor>(value);
    if (clamped != value) {
        audioBeatFluxFloor = clamped;
    }
    return clamped;
}

void AudioConfig::setBeatFluxFloor(float value) {
    audioBeatFluxFloor = audioParamClampF<kAudioParamBeatFluxFloor>(value);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioBeatFluxFloor.mark_dirty();
        persistent_value_store::request_save();
    }
}

float AudioConfig::getBeatAlpha() {
    float value = audioBeatAlpha;
    float clamped = audioParamClampF<kAudioParamBeatAlpha>(value);
    if (clamped != value) {
        audioBeatAlpha = clamped;
    }
    return clamped;
}

void AudioConfig::setBeatAlpha(float value) {
    audioBeatAlpha = audioParamClampF<kAudioParamBeatAlpha>(value);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioBeatAlpha.mark_dirty();
        persistent_value_store::request_save();
    }
}

uint32_t AudioConfig::getBeatRefractoryFrames() {
    uint32_t value = audioBeatRefractoryFrames;
    // Clamped to fit the uint8_t per-band refractory counter in audio_dsp.cpp.
    uint32_t clamped = audioParamClampU<kAudioParamBeatRefractoryFrames>(value);
    if (clamped != value) {
        audioBeatRefractoryFrames = clamped;
    }
    return clamped;
}

void AudioConfig::setBeatRefractoryFrames(uint32_t value) {
    // Clamped to fit the uint8_t per-band refractory counter in audio_dsp.cpp.
    audioBeatRefractoryFrames = audioParamClampU<kAudioParamBeatRefractoryFrames>(value);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioBeatRefractoryFrames.mark_dirty();
        persistent_value_store::request_save();
    }
}

float AudioConfig::getSfDelta() {
    float value = audioSfDelta;
    float clamped = audioParamClampF<kAudioParamSfDelta>(value);
    if (clamped != value) {
        audioSfDelta = clamped;
    }
    return clamped;
}

void AudioConfig::setSfDelta(float value) {
    audioSfDelta = audioParamClampF<kAudioParamSfDelta>(value);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioSfDelta.mark_dirty();
        persistent_value_store::request_save();
    }
}

uint32_t AudioConfig::getThresholdMode() {
    uint32_t value = audioThresholdMode;
    uint32_t clamped = audioParamClampU<kAudioParamThresholdMode>(value);
    if (clamped != value) {
        audioThresholdMode = clamped;
    }
    return clamped;
}

void AudioConfig::setThresholdMode(uint32_t value) {
    audioThresholdMode = audioParamClampU<kAudioParamThresholdMode>(value);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioThresholdMode.mark_dirty();
        persistent_value_store::request_save();
    }
}

float AudioConfig::getTargetLow() {
    float value = audioAgcTargetLow;
    float clamped = audioParamClampF<kAudioParamAgcTargetLow>(value);
    if (clamped != value) {
        audioAgcTargetLow = clamped;
    }
    return clamped;
}

void AudioConfig::setTargetLow(float value) {
    audioAgcTargetLow = audioParamClampF<kAudioParamAgcTargetLow>(value);
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
    float clamped = audioParamClampF<kAudioParamAgcTargetHigh>(value);
    if (clamped != value) {
        audioAgcTargetHigh = clamped;
    }
    return clamped;
}

void AudioConfig::setTargetHigh(float value) {
    audioAgcTargetHigh = audioParamClampF<kAudioParamAgcTargetHigh>(value);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioAgcTargetHigh.mark_dirty();
        persistent_value_store::request_save();
    }
}

uint32_t AudioConfig::getAttackFrames() {
    uint32_t value = audioAgcAttackFrames;
    uint32_t clamped = audioParamClampU<kAudioParamAgcAttackFrames>(value);
    if (clamped != value) {
        audioAgcAttackFrames = clamped;
    }
    return clamped;
}

void AudioConfig::setAttackFrames(uint32_t value) {
    audioAgcAttackFrames = audioParamClampU<kAudioParamAgcAttackFrames>(value);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioAgcAttackFrames.mark_dirty();
        persistent_value_store::request_save();
    }
}

uint32_t AudioConfig::getReleaseFrames() {
    uint32_t value = audioAgcReleaseFrames;
    uint32_t clamped = audioParamClampU<kAudioParamAgcReleaseFrames>(value);
    if (clamped != value) {
        audioAgcReleaseFrames = clamped;
    }
    return clamped;
}

void AudioConfig::setReleaseFrames(uint32_t value) {
    audioAgcReleaseFrames = audioParamClampU<kAudioParamAgcReleaseFrames>(value);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioAgcReleaseFrames.mark_dirty();
        persistent_value_store::request_save();
    }
}

float AudioConfig::getNoiseGateRms() {
    float value = audioNoiseGateRms;
    float clamped = audioParamClampF<kAudioParamNoiseGateRms>(value);
    if (clamped != value) {
        audioNoiseGateRms = clamped;
    }
    return clamped;
}

void AudioConfig::setNoiseGateRms(float value) {
    audioNoiseGateRms = audioParamClampF<kAudioParamNoiseGateRms>(value);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioNoiseGateRms.mark_dirty();
        persistent_value_store::request_save();
    }
}

uint32_t AudioConfig::getRateLimitFrames() {
    uint32_t value = audioAgcRateLimitFrames;
    uint32_t clamped = audioParamClampU<kAudioParamAgcRateLimitFrames>(value);
    if (clamped != value) {
        audioAgcRateLimitFrames = clamped;
    }
    return clamped;
}

void AudioConfig::setRateLimitFrames(uint32_t value) {
    audioAgcRateLimitFrames = audioParamClampU<kAudioParamAgcRateLimitFrames>(value);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioAgcRateLimitFrames.mark_dirty();
        persistent_value_store::request_save();
    }
}

float AudioConfig::getSmoothingCoeff() const {
    float value = audioFftSmoothingCoeff;
    float clamped = audioParamClampF<kAudioParamFftSmoothingCoeff>(value);
    if (clamped != value) {
        audioFftSmoothingCoeff = clamped;
    }
    return clamped;
}

float AudioConfig::getEnergyScale() const {
    float value = audioFftEnergyScale;
    float clamped = audioParamClampF<kAudioParamFftEnergyScale>(value);
    if (clamped != value) {
        audioFftEnergyScale = clamped;
    }
    return clamped;
}

void audio_dsp_bind_default_bt_dependencies() {
    audio_dsp_set_config_provider(&AudioConfig::getInstance());
    sound_set_agc_config_provider(&AudioConfig::getInstance());

    /* Read every parameter once so a persisted out-of-range or non-finite value is corrected
     * NOW rather than whenever its consumer happens to run.
     *
     * The getters clamp on read and write the clamped value back, which is the migration
     * mechanism — but that only fires when something calls them. Twelve of these are read by
     * the DSP thread every 32 ms frame, so they self-heal almost immediately. The two FFT
     * parameters are read ONLY by FftBarsAnimation::tick(), i.e. only while that one animation
     * is selected, so without this a bad persisted value for them would survive indefinitely
     * (and be re-flushed to NVS by the settings debounce) on a device running any other
     * animation. Cheap insurance: one read each, once, at bind time.
     *
     * This does NOT close the write-side window — a remote write still lands in storage_
     * unvalidated and reads back verbatim until the next getter call, because
     * BtGattPersistentCharacteristic uses the plain onWrite hook and the framework forbids
     * defining both onWrite and onWriteChecked on one type. Rejecting non-finite writes at the
     * ATT boundary needs that framework change and is deliberately out of scope here. */
    AudioConfig &cfg = AudioConfig::getInstance();
    (void)cfg.getFluxGamma();
    (void)cfg.getBeatFluxFloor();
    (void)cfg.getBeatAlpha();
    (void)cfg.getBeatRefractoryFrames();
    (void)cfg.getSfDelta();
    (void)cfg.getThresholdMode();
    (void)cfg.getTargetLow();
    (void)cfg.getTargetHigh();
    (void)cfg.getRateLimitFrames();
    (void)cfg.getAttackFrames();
    (void)cfg.getReleaseFrames();
    (void)cfg.getNoiseGateRms();
    (void)cfg.getSmoothingCoeff();
    (void)cfg.getEnergyScale();
}
