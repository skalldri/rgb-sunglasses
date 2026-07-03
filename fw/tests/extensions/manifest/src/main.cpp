/*
 * extensions.manifest — native_sim tests for the pure extension manifest
 * validator (extension_manifest.cpp). The validator is the security gate
 * between untrusted .llext files and kernel-mode pointer dereferences; these
 * tests cover every rejection path plus the copy-out semantics.
 *
 * The "extension memory" is simulated with a byte arena: the InRegion
 * predicate accepts only pointers inside it, so tests can place manifest
 * pieces inside/outside at will.
 */

#include <extensions/extension_manifest.h>
#include <zephyr/ztest.h>

#include <cstring>

using extension_manifest::Env;
using extension_manifest::Metadata;
using extension_manifest::Result;
using extension_manifest::validate;

namespace {

/* Simulated extension memory. Everything the manifest references must live
 * in here for the fake InRegion predicate to accept it. */
struct Arena {
    alignas(8) uint8_t bytes[2048];
    size_t used = 0;

    void *alloc(size_t n) {
        zassert_true(used + n <= sizeof(bytes), "arena overflow");
        void *p = &bytes[used];
        used = (used + n + 7) & ~size_t{7};
        return p;
    }

    const char *str(const char *s) {
        char *p = static_cast<char *>(alloc(strlen(s) + 1));
        strcpy(p, s);
        return p;
    }
};

Arena sArena;

bool arena_in_region(void *ctx, const void *ptr, size_t len) {
    const auto *a = static_cast<const Arena *>(ctx);
    const auto p = reinterpret_cast<uintptr_t>(ptr);
    const auto base = reinterpret_cast<uintptr_t>(a->bytes);
    return len > 0 && p >= base && p + len <= base + sizeof(a->bytes);
}

constexpr uint32_t kW = 40;
constexpr uint32_t kH = 12;

Env test_env() {
    return Env{
        .expectedWidth = kW,
        .expectedHeight = kH,
        .inRegion = arena_in_region,
        .ctx = &sArena,
    };
}

/* Builds a well-formed manifest inside the arena; tests then poke fields. */
struct rgbx_manifest *make_manifest(const struct rgbx_param_desc *params, uint32_t count) {
    auto *m = static_cast<struct rgbx_manifest *>(sArena.alloc(sizeof(struct rgbx_manifest)));
    *m = {
        .abi_version = RGBX_ABI_VERSION,
        .name = sArena.str("Test Extension"),
        .width = kW,
        .height = kH,
        .param_count = count,
        .params = params,
    };
    return m;
}

struct rgbx_param_desc *arena_params(size_t count) {
    return static_cast<struct rgbx_param_desc *>(
        sArena.alloc(count * sizeof(struct rgbx_param_desc)));
}

void reset_arena() {
    sArena.used = 0;
}

}  // namespace

ZTEST(extension_manifest_suite, test_happy_path_copies_everything) {
    reset_arena();
    auto *params = arena_params(5);
    params[0] = RGBX_PARAM("Speed", RGBX_PARAM_UINT32, 50);
    params[1] = RGBX_PARAM("Color", RGBX_PARAM_COLOR, 0x00FF40FFu);
    params[2] = RGBX_PARAM("Crash", RGBX_PARAM_BOOL, 7); /* clamped to 1 */
    params[3] = {sArena.str("Message"), RGBX_PARAM_STRING, {.str = sArena.str("HELLO")}};
    params[4] = {sArena.str("Label"), RGBX_PARAM_STRING, {.str = nullptr}};
    /* param name pointers for the scalar macros are string literals OUTSIDE
     * the arena — fix them up to arena copies. */
    params[0].name = sArena.str("Speed");
    params[1].name = sArena.str("Color");
    params[2].name = sArena.str("Crash");
    auto *m = make_manifest(params, 5);

    Metadata out;
    zassert_equal(validate(m, test_env(), out), Result::Ok);
    zassert_equal(strcmp(out.displayName, "Test Extension"), 0);
    zassert_equal(out.width, kW);
    zassert_equal(out.height, kH);
    zassert_equal(out.paramCount, 5u);
    zassert_equal(out.stringParamCount, 2u);
    zassert_equal(strcmp(out.params[0].name, "Speed"), 0);
    zassert_equal(out.params[0].type, RGBX_PARAM_UINT32);
    zassert_equal(out.params[0].defaultValue, 50u);
    zassert_equal(out.params[1].defaultValue, 0x00FF40FFu);
    zassert_equal(out.params[2].defaultValue, 1u, "BOOL default must be clamped to 0/1");
    zassert_equal(out.params[3].stringSlot, 0);
    zassert_equal(strcmp(out.stringDefaults[0], "HELLO"), 0);
    zassert_equal(out.params[4].stringSlot, 1);
    zassert_equal(out.stringDefaults[1][0], '\0', "NULL string default reads as empty");
    zassert_equal(out.params[0].stringSlot, extension_manifest::kNoStringSlot);
}

ZTEST(extension_manifest_suite, test_abi_version_mismatch_rejected) {
    reset_arena();
    auto *m = make_manifest(nullptr, 0);
    m->abi_version = RGBX_ABI_VERSION + 1;
    Metadata out;
    zassert_equal(validate(m, test_env(), out), Result::BadAbiVersion);
}

ZTEST(extension_manifest_suite, test_dims_mismatch_rejected) {
    reset_arena();
    auto *m = make_manifest(nullptr, 0);
    m->width = kW + 1;
    Metadata out;
    zassert_equal(validate(m, test_env(), out), Result::BadDims);
}

ZTEST(extension_manifest_suite, test_manifest_outside_region_rejected) {
    reset_arena();
    struct rgbx_manifest onStack = {}; /* lives outside the arena */
    onStack.abi_version = RGBX_ABI_VERSION;
    Metadata out;
    zassert_equal(validate(&onStack, test_env(), out), Result::BadManifestPointer);
    zassert_equal(validate(nullptr, test_env(), out), Result::BadManifestPointer);
}

ZTEST(extension_manifest_suite, test_param_count_over_max_rejected) {
    reset_arena();
    auto *params = arena_params(1);
    params[0] = RGBX_PARAM("p", RGBX_PARAM_UINT32, 0);
    auto *m = make_manifest(params, RGBX_MAX_PARAMS + 1);
    Metadata out;
    zassert_equal(validate(m, test_env(), out), Result::BadParamTable);
}

ZTEST(extension_manifest_suite, test_params_null_iff_count_zero) {
    reset_arena();
    /* count == 0 but params != NULL */
    auto *params = arena_params(1);
    auto *m = make_manifest(params, 0);
    Metadata out;
    zassert_equal(validate(m, test_env(), out), Result::BadParamTable);
    /* count > 0 but params == NULL */
    auto *m2 = make_manifest(nullptr, 1);
    zassert_equal(validate(m2, test_env(), out), Result::BadParamTable);
}

ZTEST(extension_manifest_suite, test_param_table_outside_region_rejected) {
    reset_arena();
    static struct rgbx_param_desc outside[1]; /* static: outside the arena */
    outside[0] = RGBX_PARAM("p", RGBX_PARAM_UINT32, 0);
    auto *m = make_manifest(outside, 1);
    Metadata out;
    zassert_equal(validate(m, test_env(), out), Result::BadParamTable);
}

ZTEST(extension_manifest_suite, test_wild_name_pointers_rejected) {
    reset_arena();
    /* manifest name outside the arena */
    auto *m = make_manifest(nullptr, 0);
    m->name = "literal outside arena";
    Metadata out;
    zassert_equal(validate(m, test_env(), out), Result::BadName);

    /* param name outside the arena */
    auto *params = arena_params(1);
    params[0] = {"literal outside arena", RGBX_PARAM_UINT32, {.u32 = 0}};
    auto *m2 = make_manifest(params, 1);
    zassert_equal(validate(m2, test_env(), out), Result::BadParamName);
}

ZTEST(extension_manifest_suite, test_null_name_reads_unnamed) {
    reset_arena();
    auto *m = make_manifest(nullptr, 0);
    m->name = nullptr;
    Metadata out;
    zassert_equal(validate(m, test_env(), out), Result::Ok);
    zassert_equal(strcmp(out.displayName, "unnamed"), 0);
}

ZTEST(extension_manifest_suite, test_long_name_truncated_with_nul) {
    reset_arena();
    auto *m = make_manifest(nullptr, 0);
    m->name = sArena.str("This display name is much longer than the 24-byte buffer");
    Metadata out;
    zassert_equal(validate(m, test_env(), out), Result::Ok);
    zassert_equal(strlen(out.displayName), sizeof(out.displayName) - 1,
                  "must truncate to buffer capacity");
    zassert_equal(out.displayName[sizeof(out.displayName) - 1], '\0');
}

ZTEST(extension_manifest_suite, test_unterminated_name_rejected) {
    reset_arena();
    auto *m = make_manifest(nullptr, 0);
    /* A "string" with no NUL anywhere in the remaining arena: point at a
     * fully 'A'-filled tail of the arena. */
    char *tail = static_cast<char *>(sArena.alloc(64));
    memset(tail, 'A', 64);
    memset(sArena.bytes + sArena.used, 'A', sizeof(sArena.bytes) - sArena.used);
    m->name = tail;
    Metadata out;
    zassert_equal(validate(m, test_env(), out), Result::BadName);
}

ZTEST(extension_manifest_suite, test_bad_param_type_rejected) {
    reset_arena();
    auto *params = arena_params(1);
    params[0] = {sArena.str("p"), static_cast<enum rgbx_param_type>(99), {.u32 = 0}};
    auto *m = make_manifest(params, 1);
    Metadata out;
    zassert_equal(validate(m, test_env(), out), Result::BadParamType);
}

ZTEST(extension_manifest_suite, test_too_many_string_params_rejected) {
    reset_arena();
    const size_t n = RGBX_MAX_STRING_PARAMS + 1;
    auto *params = arena_params(n);
    for (size_t i = 0; i < n; i++) {
        params[i] = {sArena.str("s"), RGBX_PARAM_STRING, {.str = nullptr}};
    }
    auto *m = make_manifest(params, n);
    Metadata out;
    zassert_equal(validate(m, test_env(), out), Result::TooManyStringParams);
}

ZTEST(extension_manifest_suite, test_bad_string_defaults_rejected) {
    reset_arena();
    /* default outside the arena */
    auto *params = arena_params(1);
    params[0] = {sArena.str("s"), RGBX_PARAM_STRING, {.str = "outside arena"}};
    auto *m = make_manifest(params, 1);
    Metadata out;
    zassert_equal(validate(m, test_env(), out), Result::BadStringDefault);

    /* overlong default (>= RGBX_PARAM_STRING_MAX bytes): must reject, not
     * truncate — values round-trip through BLE unmodified. */
    auto *params2 = arena_params(1);
    params2[0] = {sArena.str("s"), RGBX_PARAM_STRING,
                  {.str = sArena.str("0123456789012345678901234567890123456789")}};
    auto *m2 = make_manifest(params2, 1);
    zassert_equal(validate(m2, test_env(), out), Result::BadStringDefault);
}

ZTEST_SUITE(extension_manifest_suite, NULL, NULL, NULL, NULL, NULL);
