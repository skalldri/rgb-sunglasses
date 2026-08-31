#include <cstdio>
#include <cstring>
#include "sound/audio_telemetry_codec.h"

static void emit(const char *name, const audio_telemetry_frame &f, uint8_t tier) {
    uint8_t buf[64];
    size_t n = audio_telemetry_pack(&f, tier, buf, sizeof(buf));
    printf("  {\n    \"name\": \"%s\",\n    \"tier\": %u,\n    \"bytes\": [", name, tier);
    for (size_t i = 0; i < n; i++) printf("%s%u", i ? ", " : "", buf[i]);
    printf("],\n");
    printf("    \"expect\": { \"seq\": %u, \"dropped\": %u, \"gainSteps\": %d, "
           "\"clipCount\": %u, \"framesSinceStep\": %u, \"silent\": %s, \"clipped\": %s, "
           "\"agcFrozen\": %s, \"thresholdMode\": %u, \"beatMask\": %u,\n",
           f.seq, f.dropped, f.gain_steps, f.clip_count, f.frames_since_step,
           f.silent ? "true" : "false", f.clipped ? "true" : "false",
           f.agc_frozen ? "true" : "false", f.threshold_mode, f.beat_mask);
    printf("      \"rmsInput\": %.9g, \"rmsInstant\": %.9g, \"peak\": %.9g, \"noiseFloor\": %.9g,\n",
           audio_telemetry_dq_log(audio_telemetry_q_log(f.rms_input_referred)),
           audio_telemetry_dq_log(audio_telemetry_q_log(f.rms_instant)),
           audio_telemetry_dq_log(audio_telemetry_q_log(f.peak)),
           audio_telemetry_dq_log(audio_telemetry_q_log(f.noise_floor)));
    printf("      \"flux\": [");
    for (int b = 0; b < AUDIO_NUM_BANDS; b++)
        printf("%s%.9g", b ? ", " : "", audio_telemetry_dq_log(audio_telemetry_q_log(f.flux[b])));
    printf("],\n      \"threshold\": [");
    for (int b = 0; b < AUDIO_NUM_BANDS; b++)
        printf("%s%.9g", b ? ", " : "", audio_telemetry_dq_log(audio_telemetry_q_log(f.threshold[b])));
    printf("]\n    }\n  }");
}

int main() {
    printf("[\n");
    audio_telemetry_frame f{};
    f.seq = 0x1234; f.dropped = 7; f.gain_steps = 13;
    f.rms_input_referred = 0.02f; f.rms_instant = 0.031f; f.peak = 0.5f; f.noise_floor = 0.0006f;
    f.clip_count = 9; f.frames_since_step = 55;
    f.silent = false; f.clipped = true; f.agc_frozen = false; f.threshold_mode = 0;
    f.beat_mask = 0b0101;
    float fl[4] = {0.4f, 0.2f, 0.11f, 0.05f}; memcpy(f.flux, fl, sizeof(fl));
    float th[4] = {0.3f, 0.25f, 0.2f, 0.15f}; memcpy(f.threshold, th, sizeof(th));
    float mn[4] = {0.12f, 0.22f, 0.32f, 0.42f}; memcpy(f.mean, mn, sizeof(mn));
    float sg[4] = {0.011f, 0.021f, 0.031f, 0.041f}; memcpy(f.sigma, sg, sizeof(sg));
    for (int i = 0; i < AUDIO_NUM_DISPLAY_BUCKETS; i++) f.buckets[i] = 0.01f * (i + 1);
    emit("typical_meters", f, 1); printf(",\n");
    emit("typical_stats", f, 2); printf(",\n");
    emit("typical_spectrum", f, 3); printf(",\n");

    audio_telemetry_frame g{};
    g.seq = 0xFFFF; g.dropped = 255; g.gain_steps = -40;
    g.rms_input_referred = 0.0f; g.rms_instant = 0.0f; g.peak = 0.0f; g.noise_floor = 0.0f;
    g.clip_count = 255; g.frames_since_step = 255;
    g.silent = true; g.clipped = false; g.agc_frozen = true; g.threshold_mode = 1;
    g.beat_mask = 0b1111;
    emit("extremes_silent", g, 1); printf(",\n");

    audio_telemetry_frame h{};
    h.seq = 1; h.gain_steps = 40; h.peak = 1.0f; h.rms_input_referred = 1.0f;
    h.rms_instant = 3.5f; h.noise_floor = 1e-5f;
    float hf[4] = {3.5f, 1.0f, 0.001f, 0.0f}; memcpy(h.flux, hf, sizeof(hf));
    emit("extremes_loud", h, 1);
    printf("\n]\n");
    return 0;
}
