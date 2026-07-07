/*
 * extensions.param_persistence — native_sim tests for the pure helpers
 * backing extension parameter persistence (extension_param_persistence.cpp):
 * the settings-key sanitizer and the Blob fill/apply round trip. These are
 * the only pieces of the persistence feature testable without hardware,
 * since extension_host.cpp itself requires LLEXT + USERSPACE +
 * FAT_FILESYSTEM_ELM (not native_sim-viable).
 */

#include <extensions/extension_param_persistence.h>
#include <zephyr/ztest.h>

#include <cstring>

using extension_manifest::Metadata;
using extension_manifest::ParamInfo;
using extension_param_persistence::apply_blob;
using extension_param_persistence::Blob;
using extension_param_persistence::build_settings_key;
using extension_param_persistence::fill_blob;

namespace {

Metadata make_metadata(size_t paramCount, size_t stringParamCount) {
    Metadata meta{};
    meta.paramCount = paramCount;
    meta.stringParamCount = stringParamCount;
    for (size_t p = 0; p < paramCount; p++) {
        meta.params[p].type = RGBX_PARAM_UINT32;
        meta.params[p].defaultValue = 0;
    }
    return meta;
}

}  // namespace

ZTEST(extension_param_persistence_suite, test_build_settings_key_normal_name) {
    char out[extension_param_persistence::kKeyMaxLen];
    build_settings_key(out, sizeof(out), "MyExtension");
    zassert_equal(strcmp(out, "ext/MyExtension"), 0);
}

ZTEST(extension_param_persistence_suite, test_build_settings_key_sanitizes_slash_and_equals) {
    char out[extension_param_persistence::kKeyMaxLen];
    build_settings_key(out, sizeof(out), "Foo/Bar=Baz");
    zassert_equal(strcmp(out, "ext/Foo_Bar_Baz"), 0, "got '%s'", out);
}

ZTEST(extension_param_persistence_suite, test_build_settings_key_empty_name) {
    char out[extension_param_persistence::kKeyMaxLen];
    build_settings_key(out, sizeof(out), "");
    zassert_equal(strcmp(out, "ext/"), 0);
}

ZTEST(extension_param_persistence_suite, test_build_settings_key_truncates_and_nul_terminates) {
    char out[8];  // room for "ext/" (4) + 3 chars + NUL
    build_settings_key(out, sizeof(out), "ThisNameIsWayTooLongToFit");
    zassert_equal(strlen(out), 7u, "expected exactly 7 chars before the NUL, got '%s'", out);
    zassert_equal(out[7], '\0');
    zassert_equal(strncmp(out, "ext/", 4), 0);
}

ZTEST(extension_param_persistence_suite, test_build_settings_key_zero_length_buffer_is_noop) {
    char out[1] = {'x'};
    build_settings_key(out, 0, "whatever");
    zassert_equal(out[0], 'x', "a zero-length buffer must not be touched at all");
}

ZTEST(extension_param_persistence_suite, test_fill_and_apply_blob_round_trip) {
    Metadata meta = make_metadata(2, 1);
    meta.params[1].type = RGBX_PARAM_COLOR;

    uint32_t savedParams[RGBX_MAX_PARAMS] = {42, 0x00FF8800u};
    char savedStrings[RGBX_MAX_STRING_PARAMS][RGBX_PARAM_STRING_MAX] = {};
    strcpy(savedStrings[0], "hello");

    Blob blob;
    fill_blob(blob, savedParams, savedStrings);

    uint32_t loadedParams[RGBX_MAX_PARAMS] = {};
    char loadedStrings[RGBX_MAX_STRING_PARAMS][RGBX_PARAM_STRING_MAX] = {};
    apply_blob(blob, meta, loadedParams, loadedStrings);

    zassert_equal(loadedParams[0], 42u);
    zassert_equal(loadedParams[1], 0x00FF8800u);
    zassert_equal(strcmp(loadedStrings[0], "hello"), 0);
}

ZTEST(extension_param_persistence_suite, test_apply_blob_clamps_bool_params) {
    Metadata meta = make_metadata(1, 0);
    meta.params[0].type = RGBX_PARAM_BOOL;

    uint32_t savedParams[RGBX_MAX_PARAMS] = {7};  // garbage, must clamp to 1
    char savedStrings[RGBX_MAX_STRING_PARAMS][RGBX_PARAM_STRING_MAX] = {};
    Blob blob;
    fill_blob(blob, savedParams, savedStrings);

    uint32_t loadedParams[RGBX_MAX_PARAMS] = {};
    char loadedStrings[RGBX_MAX_STRING_PARAMS][RGBX_PARAM_STRING_MAX] = {};
    apply_blob(blob, meta, loadedParams, loadedStrings);

    zassert_equal(loadedParams[0], 1u, "BOOL param must be clamped to 0/1");
}

ZTEST(extension_param_persistence_suite, test_apply_blob_only_touches_valid_indices) {
    // meta only describes 1 param / 0 string params - a stale blob (e.g. from
    // before an extension update shrank its param table) must not touch
    // anything beyond that, even though the blob itself carries full-size
    // arrays with leftover data in the unused slots.
    Metadata meta = make_metadata(1, 0);

    uint32_t savedParams[RGBX_MAX_PARAMS] = {10, 20, 30};
    char savedStrings[RGBX_MAX_STRING_PARAMS][RGBX_PARAM_STRING_MAX] = {};
    strcpy(savedStrings[0], "stale");
    Blob blob;
    fill_blob(blob, savedParams, savedStrings);

    uint32_t loadedParams[RGBX_MAX_PARAMS] = {0, 0xDEADBEEFu, 0xDEADBEEFu};
    char loadedStrings[RGBX_MAX_STRING_PARAMS][RGBX_PARAM_STRING_MAX] = {};
    strcpy(loadedStrings[0], "untouched");
    apply_blob(blob, meta, loadedParams, loadedStrings);

    zassert_equal(loadedParams[0], 10u, "the one valid param index must be applied");
    zassert_equal(loadedParams[1], 0xDEADBEEFu, "indices beyond paramCount must be untouched");
    zassert_equal(loadedParams[2], 0xDEADBEEFu, "indices beyond paramCount must be untouched");
    zassert_equal(strcmp(loadedStrings[0], "untouched"), 0,
                  "string slots beyond stringParamCount must be untouched");
}

ZTEST(extension_param_persistence_suite, test_apply_blob_force_nul_terminates_strings) {
    Metadata meta = make_metadata(0, 1);

    uint32_t savedParams[RGBX_MAX_PARAMS] = {};
    char savedStrings[RGBX_MAX_STRING_PARAMS][RGBX_PARAM_STRING_MAX];
    memset(savedStrings[0], 'A', RGBX_PARAM_STRING_MAX);  // no NUL anywhere
    Blob blob;
    fill_blob(blob, savedParams, savedStrings);

    uint32_t loadedParams[RGBX_MAX_PARAMS] = {};
    char loadedStrings[RGBX_MAX_STRING_PARAMS][RGBX_PARAM_STRING_MAX] = {};
    apply_blob(blob, meta, loadedParams, loadedStrings);

    zassert_equal(loadedStrings[0][RGBX_PARAM_STRING_MAX - 1], '\0',
                  "the last byte must always be forced to NUL");
}

ZTEST_SUITE(extension_param_persistence_suite, NULL, NULL, NULL, NULL, NULL);
