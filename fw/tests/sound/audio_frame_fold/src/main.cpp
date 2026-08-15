#include <sound/animation_adapters/audio_frame_fold.h>
#include <zephyr/ztest.h>

namespace {
audio_analysis_result make_frame(float energy, bool beat0, bool beat2 = false) {
    audio_analysis_result frame = {};
    frame.band_energy[0] = energy;
    frame.beat[0] = beat0;
    frame.beat[2] = beat2;
    return frame;
}
}  // namespace

ZTEST_SUITE(audio_frame_fold_tests, NULL, NULL, NULL, NULL, NULL);

/* A single-frame batch behaves exactly like plain assignment. */
ZTEST(audio_frame_fold_tests, test_single_frame_replaces_cache) {
    audio_analysis_result cache = make_frame(0.5f, true);
    audio_frame_fold(cache, make_frame(0.9f, false), /*firstInBatch=*/true);

    zassert_equal(cache.band_energy[0], 0.9f, "Latest frame's fields must win");
    zassert_false(cache.beat[0], "First frame of a batch must clear the previous tick's beat");
}

/* Issue #376: at a ~33 ms render tick two ~32 ms analysis frames can arrive in one
 * drain; a beat in the OLDER frame must survive the newer beat-less frame. */
ZTEST(audio_frame_fold_tests, test_beat_in_older_frame_survives_batch) {
    audio_analysis_result cache = {};
    audio_frame_fold(cache, make_frame(0.4f, true), /*firstInBatch=*/true);
    audio_frame_fold(cache, make_frame(0.8f, false), /*firstInBatch=*/false);

    zassert_true(cache.beat[0], "The older frame's beat must OR into the batch result");
    zassert_equal(cache.band_energy[0], 0.8f,
                  "Non-beat fields must still come from the newest frame");
}

/* Beats OR per band independently across the batch. */
ZTEST(audio_frame_fold_tests, test_beats_or_per_band) {
    audio_analysis_result cache = {};
    audio_frame_fold(cache, make_frame(0.1f, true, false), /*firstInBatch=*/true);
    audio_frame_fold(cache, make_frame(0.2f, false, true), /*firstInBatch=*/false);

    zassert_true(cache.beat[0], "Band 0 beat from frame 1 must survive");
    zassert_true(cache.beat[2], "Band 2 beat from frame 2 must be present");
    zassert_false(cache.beat[1], "Band 1 never beat in this batch");
    zassert_false(cache.beat[3], "Band 3 never beat in this batch");
}

/* The OR spans exactly one batch: the next batch's first frame clears stale flags. */
ZTEST(audio_frame_fold_tests, test_next_batch_clears_previous_beats) {
    audio_analysis_result cache = {};
    audio_frame_fold(cache, make_frame(0.4f, true), /*firstInBatch=*/true);
    audio_frame_fold(cache, make_frame(0.8f, false), /*firstInBatch=*/false);
    zassert_true(cache.beat[0], "Batch 1 must report its beat");

    audio_frame_fold(cache, make_frame(0.6f, false), /*firstInBatch=*/true);
    zassert_false(cache.beat[0], "Batch 2 must not inherit batch 1's beat");
}
