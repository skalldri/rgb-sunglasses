/* Emits the FFT Bars display-mapping test vectors as JSON, computed by the firmware's own
 * header (fw/src/animations/fft_bar_mapping.h, over the SDK's rgbx_audio_bars.h), so the
 * companion app's TypeScript mirror (app/services/fft-bar-mapping.ts) is pinned to the
 * exact numbers the glasses draw. It is the only consumer today; any future mirror (e.g.
 * offline capture-scoring tooling) should read the same app fixture rather than a copy.
 *
 * Regenerate after any change to the mapping, the octave table or the table defaults
 * (from the repo root):
 *
 *   g++ -std=c++2b -I fw/src -I fw/src/sound -I fw/include -o /tmp/gen_fft_bars fw/tools/gen_fft_bar_vectors.cpp -lm
 *   /tmp/gen_fft_bars > app/__tests__/fixtures/fft-bar-vectors.json
 */
#include <cstdio>

#include "animations/fft_bar_mapping.h"
#include "sound/audio_param_table.h"

static constexpr int kPanelRows = 12; /* proto0: fw/src/led_config.h kFrameDisplayHeight */

static void emit(const char *name, float power, size_t bucket, const FftBarWindow &w,
                 bool last) {
    const float h = fft_bar_height(power, bucket, w);
    const int rows = (int)(h * (float)kPanelRows + 0.5f);
    printf("    {\"name\": \"%s\", \"power\": %.9g, \"bucket\": %zu, \"floorDb\": %.9g, "
           "\"rangeDb\": %.9g, \"tiltDbPerOctave\": %.9g, \"energyScale\": %.9g, "
           "\"powerDb\": %.9g, \"fraction\": %.9g, \"rows12\": %d}%s\n",
           name, power, bucket, w.floorDb, w.rangeDb, w.tiltDbPerOctave, w.energyScale,
           fft_bar_power_db(power), h, rows, last ? "" : ",");
}

int main() {
    const FftBarWindow d = {audioParamDefaultF<kAudioParamFftFloorDb>(),
                            audioParamDefaultF<kAudioParamFftRangeDb>(),
                            audioParamDefaultF<kAudioParamFftTiltDbOct>(),
                            audioParamDefaultF<kAudioParamFftEnergyScale>()};

    printf("{\n");
    printf("  \"defaults\": {\"floorDb\": %.9g, \"rangeDb\": %.9g, \"tiltDbPerOctave\": %.9g, "
           "\"energyScale\": %.9g, \"smoothingCoeff\": %.9g, \"energyScaleUnity\": %.9g, "
           "\"powerFloor\": %.9g},\n",
           d.floorDb, d.rangeDb, d.tiltDbPerOctave, d.energyScale,
           audioParamDefaultF<kAudioParamFftSmoothingCoeff>(), kFftBarEnergyScaleUnity,
           kFftBarPowerFloor);
    printf("  \"bucketOctaves\": [");
    for (size_t b = 0; b < AUDIO_NUM_DISPLAY_BUCKETS; b++) {
        printf("%s%.9g", b ? ", " : "", rgbx_audio_bar_octaves[b]);
    }
    printf("],\n");
    printf("  \"vectors\": [\n");

    emit("zero_power", 0.0f, 0, d, false);
    emit("negative_power", -1.0f, 0, d, false);
    emit("tiny_power", 1e-9f, 0, d, false);
    emit("at_floor", 2.51188643e-4f, 0, d, false);     /* −36 dB */
    emit("mid_window", 1.58489319e-2f, 0, d, false);   /* −18 dB */
    emit("at_ceiling", 1.0f, 0, d, false);              /* 0 dB */
    emit("above_ceiling", 1000.0f, 0, d, false);
    emit("tv_bucket0_median", 0.039f, 0, d, false);     /* 2026-09-03 capture */
    emit("tv_bucket0_max", 1.87f, 0, d, false);
    emit("tv_bucket19_median", 3e-5f, 19, d, false);
    emit("legacy_saturation_point", 0.05f, 0, d, false);
    emit("tilt_bucket5", 0.01f, 5, d, false);
    emit("tilt_bucket19", 0.01f, 19, d, false);

    FftBarWindow flat = d;
    flat.tiltDbPerOctave = 0.0f;
    emit("no_tilt_bucket19", 0.01f, 19, flat, false);

    FftBarWindow louder = d;
    louder.energyScale = 200.0f;
    emit("gain_plus10", 0.01f, 0, louder, false);
    FftBarWindow quieter = d;
    quieter.energyScale = 2.0f;
    emit("gain_minus10", 0.01f, 0, quieter, false);

    FftBarWindow narrow = {-20.0f, 12.0f, 0.0f, 20.0f};
    emit("narrow_window_low", 0.02f, 0, narrow, false);   /* −17 dB → 0.25 */
    emit("narrow_window_high", 0.2f, 0, narrow, false);   /* −7 dB → 1.0 */

    FftBarWindow wide = {-80.0f, 80.0f, 12.0f, 1000.0f};
    emit("extreme_window", 1e-6f, 19, wide, true);

    printf("  ]\n}\n");
    return 0;
}
