#pragma once

#include <animations/fft_bars_animation.h>
#include <singleton.h>
#include <sound/audio_dsp.h>
#include <sound/sound.h>

#include <cstddef>
#include <cstdint>

/**
 * @brief BT-backed canonical storage for every tunable audio-analysis parameter
 * (beat detection, AGC, spectrogram visualization). Mirrors CoreConfig
 * (fw/src/core_config.h) exactly: owns the BtGattPersistentCharacteristic globals in
 * audio_config.cpp and exposes them through the three provider interfaces that
 * audio_dsp.cpp, sound.cpp, and fft_bars_animation.cpp depend on instead of depending on
 * this class directly (see those headers for why - keeps native_sim tests BT-free).
 */
class AudioConfig : public Singleton<AudioConfig>,
                    public AudioDspConfigProvider,
                    public AgcConfigProvider,
                    public FftVisualizationConfigSource {
   public:
    static constexpr size_t kServiceIdNum = 2;  // CoreConfig already claims 1

    // AudioDspConfigProvider
    float getFluxGamma() override;
    float getBeatFluxFloor() override;
    float getBeatAlpha() override;
    uint32_t getBeatRefractoryFrames() override;

    // AgcConfigProvider
    float getTargetLow() override;
    void setTargetLow(float value) override;
    float getTargetHigh() override;
    void setTargetHigh(float value) override;
    uint32_t getRateLimitFrames() override;
    void setRateLimitFrames(uint32_t value) override;

    // FftVisualizationConfigSource
    float getSmoothingCoeff() const override;
    float getEnergyScale() const override;
};
