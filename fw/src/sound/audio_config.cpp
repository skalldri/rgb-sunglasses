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
BtGattPersistentCharacteristic<"audio/beat_alpha", "Beat Alpha", true, float, 3.5f> audioBeatAlpha;
BtGattPersistentCharacteristic<"audio/beat_refractory_frames", "Beat Refractory Frames", true,
                               uint32_t, 5>
    audioBeatRefractoryFrames;
BtGattPersistentCharacteristic<"audio/agc_target_low", "AGC Target Low", true, float, 0.005f>
    audioAgcTargetLow;
BtGattPersistentCharacteristic<"audio/agc_target_high", "AGC Target High", true, float, 0.008f>
    audioAgcTargetHigh;
BtGattPersistentCharacteristic<"audio/agc_rate_limit_frames", "AGC Rate Limit Frames", true,
                               uint32_t, 10>
    audioAgcRateLimitFrames;
BtGattPersistentCharacteristic<"audio/fft_smoothing_coeff", "FFT Smoothing Coeff", true, float,
                               0.3f>
    audioFftSmoothingCoeff;
BtGattPersistentCharacteristic<"audio/fft_energy_scale", "FFT Energy Scale", true, float, 20.0f>
    audioFftEnergyScale;

BtGattServer audioConfigServer(audioConfigPrimaryService, audioFluxGamma, audioBeatFluxFloor,
                               audioBeatAlpha, audioBeatRefractoryFrames, audioAgcTargetLow,
                               audioAgcTargetHigh, audioAgcRateLimitFrames, audioFftSmoothingCoeff,
                               audioFftEnergyScale);
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

float AudioConfig::getBeatFluxFloor() {
    float value = audioBeatFluxFloor;
    float clamped = std::clamp(value, 0.0f, 1.0f);
    if (clamped != value) {
        audioBeatFluxFloor = clamped;
    }
    return clamped;
}

float AudioConfig::getBeatAlpha() {
    float value = audioBeatAlpha;
    float clamped = std::clamp(value, 0.1f, 20.0f);
    if (clamped != value) {
        audioBeatAlpha = clamped;
    }
    return clamped;
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
    float clamped = std::clamp(value, 0.001f, 0.2f);
    if (clamped != value) {
        audioAgcTargetHigh = clamped;
    }
    return clamped;
}

void AudioConfig::setTargetHigh(float value) {
    audioAgcTargetHigh = std::clamp(value, 0.001f, 0.2f);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        audioAgcTargetHigh.mark_dirty();
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
