/* Emits the compiled Audio Param Ranges blob as JSON, so the app's decoder can be checked
 * against the bytes the device will really send rather than against a reading of the spec.
 * See app/__tests__/audio-param-ranges.test.ts for the regeneration command. */
#include <cstdio>
#include "sound/audio_param_blob.h"

int main() {
    printf("{\n  \"size\": %zu,\n  \"bytes\": [", kAudioParamBlobSize);
    for (size_t i = 0; i < kAudioParamBlobSize; i++) {
        printf("%s%u", i ? ", " : "", kAudioParamBlob[i]);
    }
    printf("],\n  \"expect\": [\n");
    for (size_t i = 0; i < kAudioParamCount; i++) {
        const AudioParamSpec &p = kAudioParams[i];
        printf("    { \"key\": \"%s\", \"label\": \"%s\", \"type\": %u, \"unit\": \"%s\", "
               "\"def\": %.9g, \"min\": %.9g, \"max\": %.9g, \"step\": %.9g }%s\n",
               p.key, p.label, (unsigned)p.type, p.unit, p.def, p.min, p.max, p.step,
               i + 1 < kAudioParamCount ? "," : "");
    }
    printf("  ]\n}\n");
    return 0;
}
