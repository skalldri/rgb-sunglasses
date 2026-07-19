/*
 * Tests for the compile-time BT animation adapters
 * (fw/src/bluetooth/animation_adapters/*.cpp, issue #175), compiled with the
 * real BT host headers on native_sim - same CONFIG_BT=y / no-bt_enable()
 * pattern as fw/tests/bluetooth/battery_service, per fw/CLAUDE.md's note
 * that battery_service is (as of 2026-07) the only suite exercising that
 * path.
 *
 * Every adapter here uses the compile-time `BtGattServer<Providers...>` /
 * `BT_GATT_SERVER_REGISTER` machinery (bt_service_cpp.h), so each service's
 * attribute table lives in the `bt_gatt_service_static` iterable section and
 * is found the same way battery_service's own helpers do - no bt_enable(),
 * no dynamic-db foreach (that's extension_bt.cpp's mechanism, not this one).
 *
 * fft_bars_animation_bt.cpp is deliberately NOT covered here: its
 * `..._bind_default_bt_dependencies()` calls `AudioConfig::getInstance()`,
 * which (via audio_config.cpp) pulls in `sound.cpp` - a file whose
 * file-scope `DEVICE_DT_GET(DT_NODELABEL(vm3011))`/`DT_NODELABEL(pdm0)`
 * references don't exist on native_sim's default board devicetree. That's
 * the exact gap issue #83 tracks (deferred, out of scope here) - fft_bars
 * is transitively blocked by the same thing, not by anything specific to
 * its own adapter code.
 */

#include <animations/animation_is_active_binding.h>
#include <animations/animation_renderer.h>
#include <animations/beat_animation.h>
#include <animations/glim_player_animation.h>
#include <animations/matrix_code_animation.h>
#include <animations/my_eyes_animation.h>
#include <animations/pulse_animation.h>
#include <animations/rainbow_animation.h>
#include <animations/text_animation.h>
#include <animations/tilt_animation.h>
#include <animations/zigzag_animation.h>
#include <bluetooth/bt_service_cpp.h>
#include <storage/glim_registry.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/fs/fs.h>
#include <zephyr/ztest.h>

extern "C" {
#include <ff.h>
}

#include <cstring>
#include <string_view>

/* Plain `static` file-scope functions below, NOT an anonymous namespace:
 * STRUCT_SECTION_FOREACH expands to an unqualified `extern` reference to the
 * linker-defined _bt_gatt_service_static_list_start/_end symbols, and inside
 * a C++ anonymous namespace that reference itself gets namespace-scoped -
 * the linker then looks for a mangled `(anonymous namespace)::...` symbol
 * that doesn't exist instead of the real global one. battery_service.cpp's
 * equivalent helpers use the same plain-`static` pattern for this reason. */

/* Finds the static service whose primary-service attr carries this exact
 * 128-bit UUID - the same cross-check PR1 (extension_bt) does against
 * BT_ANIMATION_SERVICE_UUID(id), just walking the STATIC iterable section
 * (STRUCT_SECTION_FOREACH) instead of the dynamic db, since every adapter
 * here registers via BT_GATT_SERVICE_DEFINE, not bt_gatt_service_register(). */
static const bt_gatt_service_static *find_service(const bt_uuid_128 &want) {
    STRUCT_SECTION_FOREACH(bt_gatt_service_static, svc) {
        const auto *svcUuid = static_cast<const bt_uuid *>(svc->attrs[0].user_data);
        if (svcUuid->type == BT_UUID_TYPE_128 &&
            memcmp(BT_UUID_128(svcUuid)->val, want.val, sizeof(want.val)) == 0) {
            return svc;
        }
    }
    return nullptr;
}

/* Returns the Nth (0-based) 128-bit-UUID, readable VALUE attr within a
 * service, in declaration order - i.e. skipping the primary-service decl and
 * every characteristic's CHRC/CUD/CPF/CCC (16-bit UUID) descriptor attrs.
 * This mirrors each *_bt.cpp's own `BtGattServer(primaryService, char0,
 * char1, ..., isActive, animationName[, nowPlaying])` provider order, the
 * same positional technique battery_service's find_value_attr/
 * find_writable_attr use (scoped to one service here instead of walking
 * every registered service). */
static const bt_gatt_attr *nth_char_value(const bt_gatt_service_static *svc, size_t index) {
    size_t seen = 0;
    for (size_t i = 0; i < svc->attr_count; i++) {
        const bt_gatt_attr *attr = &svc->attrs[i];
        if (attr->uuid != nullptr && attr->uuid->type == BT_UUID_TYPE_128 &&
            attr->read != nullptr) {
            if (seen == index) {
                return attr;
            }
            seen++;
        }
    }
    return nullptr;
}

static ssize_t do_read(const bt_gatt_attr *attr, void *buf, size_t bufLen) {
    return attr->read(nullptr, attr, buf, bufLen, 0);
}

static ssize_t do_write(const bt_gatt_attr *attr, const void *data, size_t len,
                        uint16_t offset = 0, uint8_t flags = 0) {
    return attr->write(nullptr, attr, data, len, offset, flags);
}

static std::string_view read_str(const bt_gatt_attr *attr, char *buf, size_t bufCap) {
    ssize_t n = do_read(attr, buf, bufCap - 1);
    zassert_true(n >= 0);
    buf[n] = '\0';
    return buf;
}

static uint32_t read_u32(const bt_gatt_attr *attr) {
    uint32_t v = 0;
    zassert_equal(do_read(attr, &v, sizeof(v)), sizeof(v));
    return v;
}

static uint8_t read_u8(const bt_gatt_attr *attr) {
    uint8_t v = 0xFF;
    zassert_equal(do_read(attr, &v, sizeof(v)), sizeof(v));
    return v;
}

/* Exercises the shared "N persistent uint32/color characteristics, IsActive,
 * Animation Name" shape common to zigzag/tilt/beat/pulse/rainbow/matrix_code:
 * verifies the service UUID, each persistent characteristic's compile-time
 * default plus a write/read round trip, IsActive's default+round trip, and
 * the fixed Animation Name string. `numPersistent` excludes IsActive/Name
 * (indices [0, numPersistent) are the persistent characteristics). */
/* Takes the already-computed service UUID rather than an Animation id:
 * BT_ANIMATION_SERVICE_UUID()'s braced initializer needs a compile-time
 * constant argument (a runtime enum parameter here would hit the same
 * -Wnarrowing trap extension_bt.cpp's own comment documents), so callers
 * compute it from a literal Animation::X at their own call site. */
static void check_simple_adapter(const bt_uuid_128 &svcUuid, const char *expectedName,
                                 size_t numPersistent, const uint32_t *expectedDefaults) {
    const bt_gatt_service_static *svc = find_service(svcUuid);
    zassert_not_null(svc, "service not found (UUID mismatch?)");

    for (size_t i = 0; i < numPersistent; i++) {
        const bt_gatt_attr *attr = nth_char_value(svc, i);
        zassert_not_null(attr, "missing persistent characteristic %zu", i);
        zassert_equal(read_u32(attr), expectedDefaults[i], "characteristic %zu default mismatch",
                     i);
        uint32_t newValue = expectedDefaults[i] + 1;
        zassert_equal(do_write(attr, &newValue, sizeof(newValue)), sizeof(newValue));
        zassert_equal(read_u32(attr), newValue);
        /* Oversized write is rejected. */
        uint8_t tooLong[8] = {};
        zassert_equal(do_write(attr, tooLong, sizeof(tooLong)),
                     BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN));
    }

    const bt_gatt_attr *isActiveAttr = nth_char_value(svc, numPersistent);
    zassert_not_null(isActiveAttr);
    zassert_equal(read_u8(isActiveAttr), 0);
    uint8_t one = 1;
    zassert_equal(do_write(isActiveAttr, &one, sizeof(one)), sizeof(one));
    zassert_equal(read_u8(isActiveAttr), 1);

    const bt_gatt_attr *nameAttr = nth_char_value(svc, numPersistent + 1);
    zassert_not_null(nameAttr);
    char nameBuf[32];
    zassert_true(read_str(nameAttr, nameBuf, sizeof(nameBuf)) == expectedName);
}

/* Minimal renderer so `exercise_tick` below can drive a real tick() - the
 * pixel output itself isn't the point (each animation's own DI suite
 * verifies that); the point is that tick() reads every injected
 * AnimationUint32ParameterSource/*Source's get() accessor, which is exactly
 * the "simple accessor logic" issue #175 asks this suite to cover and which
 * nothing else here reaches (setDependencies() only stores the source
 * references, it never calls into them). */
class NoOpRenderer : public AnimationRenderer {
   public:
    size_t displayWidth() const override { return 4; }
    size_t displayHeight() const override { return 4; }
    void setPixel(size_t, size_t, uint8_t, uint8_t, uint8_t) override {}
};

static NoOpRenderer sRenderer;

static void exercise_tick(BaseAnimation *animation) {
    animation->init();
    animation->tick(sRenderer, 16);
}

/* Mounts a FAT filesystem on the native_sim ram-disk (boards/native_sim.overlay)
 * and writes one minimal valid GLIM file, so test_glim_player can exercise
 * glim_player_animation_bt.cpp's "registry has files" branches
 * (buildGlimSelectionValue's loop body, GlimSelectionCharacteristic::onWrite's
 * success path) - the file layout mirrors tests/animations/glim_player_animation_di's
 * writeRgb24Glim helper, trimmed to a 1x1 single frame since the pixel content
 * itself doesn't matter here (this suite never renders a frame). */
static FATFS s_nand_fat;
static struct fs_mount_t s_nand_mnt = {
    .type = FS_FATFS,
    .mnt_point = "/NAND:",
    .fs_data = &s_nand_fat,
};

static void provision_one_glim_file(const char *name) {
    zassert_ok(fs_mkfs(FS_FATFS, (uintptr_t)"NAND", NULL, 0));
    zassert_ok(fs_mount(&s_nand_mnt));
    glim_registry::init();  // creates glim_registry::kDirectory if missing

    char path[64];
    snprintf(path, sizeof(path), "%s/%s", glim_registry::kDirectory, name);
    struct fs_file_t f;
    fs_file_t_init(&f);
    zassert_ok(fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC));
    const uint8_t hdr[24] = {
        0x4D, 0x49, 0x4C, 0x47,  // magic (LE: 0x474C494D)
        1, 24,                    // version=1, header_size=24
        3, 24,                    // format=Rgb24, fps=24
        1, 0,                     // width=1 (LE)
        1, 0,                     // height=1 (LE)
        1, 0, 0, 0,                // frameCount=1 (LE)
        24, 0, 0, 0,               // frame_data_offset=24
        0, 0, 0,                   // mono color (unused for Rgb24)
    };
    zassert_equal(fs_write(&f, hdr, sizeof(hdr)), (ssize_t)sizeof(hdr));
    const uint8_t frame[3] = {10, 20, 30};
    zassert_equal(fs_write(&f, frame, sizeof(frame)), (ssize_t)sizeof(frame));
    zassert_ok(fs_close(&f));

    glim_registry::init();  // re-scan: picks up the file just written
}

ZTEST_SUITE(animation_adapters, NULL, NULL, NULL, NULL, NULL);

ZTEST(animation_adapters, test_zigzag) {
    constexpr bt_uuid_128 kSvcUuid = BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::ZigZag));
    const uint32_t defaults[] = {200, 0xFFFFFFFF};
    check_simple_adapter(kSvcUuid, "ZigZag", 2, defaults);
    zigzag_animation_bind_default_dependencies();
    exercise_tick(ZigZagAnimation::getInstance());
    AnimationIsActiveBinding<Animation::ZigZag>::setLocalActiveState(true);
}

ZTEST(animation_adapters, test_tilt) {
    const bt_uuid_128 svcUuid = BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::Tilt));
    const bt_gatt_service_static *svc = find_service(svcUuid);
    zassert_not_null(svc);
    /* No persistent characteristics on this one - just IsActive + Name. */
    const bt_gatt_attr *isActiveAttr = nth_char_value(svc, 0);
    zassert_not_null(isActiveAttr);
    zassert_equal(read_u8(isActiveAttr), 0);
    uint8_t one = 1;
    zassert_equal(do_write(isActiveAttr, &one, sizeof(one)), sizeof(one));
    zassert_equal(read_u8(isActiveAttr), 1);
    const bt_gatt_attr *nameAttr = nth_char_value(svc, 1);
    char nameBuf[32];
    zassert_true(read_str(nameAttr, nameBuf, sizeof(nameBuf)) == "Tilt");
    /* Documented no-op (TiltAnimation has no BT-backed parameters) - just
     * confirm it doesn't crash. */
    tilt_animation_bind_default_bt_dependencies();
    AnimationIsActiveBinding<Animation::Tilt>::setLocalActiveState(true);
}

ZTEST(animation_adapters, test_beat) {
    constexpr bt_uuid_128 kSvcUuid = BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::Beat));
    const uint32_t defaults[] = {0xFFFFFFFF};
    check_simple_adapter(kSvcUuid, "Beat", 1, defaults);
    beat_animation_bind_default_bt_dependencies();
    /* No exercise_tick() here: BeatAnimation::tick() early-returns whenever
     * its audio source is unset (audioSource_ is only wired by the sound
     * subsystem's own bind function, out of scope for this BT-layer suite),
     * so it would never reach color_->get() anyway. */
    AnimationIsActiveBinding<Animation::Beat>::setLocalActiveState(true);
}

ZTEST(animation_adapters, test_pulse) {
    constexpr bt_uuid_128 kSvcUuid = BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::Pulse));
    const uint32_t defaults[] = {0xFFFFFFFF, 2000};
    check_simple_adapter(kSvcUuid, "Pulse", 2, defaults);
    pulse_animation_bind_default_dependencies();
    exercise_tick(PulseAnimation::getInstance());
    AnimationIsActiveBinding<Animation::Pulse>::setLocalActiveState(true);
}

ZTEST(animation_adapters, test_rainbow) {
    constexpr bt_uuid_128 kSvcUuid = BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::Rainbow));
    const uint32_t defaults[] = {100, 5};
    check_simple_adapter(kSvcUuid, "Rainbow", 2, defaults);
    rainbow_animation_bind_default_dependencies();
    exercise_tick(RainbowAnimation::getInstance());
    AnimationIsActiveBinding<Animation::Rainbow>::setLocalActiveState(true);
}

ZTEST(animation_adapters, test_matrix_code) {
    constexpr bt_uuid_128 kSvcUuid =
        BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::MatrixCode));
    const uint32_t defaults[] = {80, 600, 40, 0x0000FF41};
    check_simple_adapter(kSvcUuid, "Matrix Code", 4, defaults);
    matrix_code_animation_bind_default_dependencies();
    exercise_tick(MatrixCodeAnimation::getInstance());
    AnimationIsActiveBinding<Animation::MatrixCode>::setLocalActiveState(true);
}

/* text/my_eyes share the same shape: step/blink time, color, up_next, 20
 * string slots (pre-seeded from a static message table at static-init time,
 * before this test runs), IsActive, Animation Name (text also has a trailing
 * "Now Playing" characteristic my_eyes doesn't). */
ZTEST(animation_adapters, test_text) {
    const bt_uuid_128 svcUuid = BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::Text));
    const bt_gatt_service_static *svc = find_service(svcUuid);
    zassert_not_null(svc);

    zassert_equal(read_u32(nth_char_value(svc, 0)), 50);          // step_time_ms
    zassert_equal(read_u32(nth_char_value(svc, 1)), 0xFFFFFFFFu); // color
    zassert_equal(read_u32(nth_char_value(svc, 2)), 0);           // up_next

    /* Slot 0 is pre-seeded from kStaticMessages[0]. */
    char slotBuf[TextAnimation::kMaxMsgLen];
    const bt_gatt_attr *slot0 = nth_char_value(svc, 3);
    zassert_not_null(slot0);
    zassert_true(read_str(slot0, slotBuf, sizeof(slotBuf)) ==
                "LIFE IS MADE OF LITTLE MOMENTS LIKE THIS");
    /* Round trip a new value. */
    const char newMsg[] = "HELLO NATIVE SIM";
    zassert_equal(do_write(slot0, newMsg, sizeof(newMsg) - 1), sizeof(newMsg) - 1);
    zassert_true(read_str(slot0, slotBuf, sizeof(slotBuf)) == "HELLO NATIVE SIM");

    /* Round-trip every remaining slot too - getTextSlot()/setTextSlot() are
     * hand-written switch statements over all kNumStringSlots cases (one
     * BtGattPersistentCharacteristic each), not something a single-slot
     * check exercises. */
    for (size_t i = 1; i < TextAnimation::kNumStringSlots; i++) {
        const bt_gatt_attr *slot = nth_char_value(svc, 3 + i);
        zassert_not_null(slot, "missing text slot %zu", i);
        char msg[8];
        int n = snprintf(msg, sizeof(msg), "S%zu", i);
        zassert_equal(do_write(slot, msg, n), n);
        zassert_true(read_str(slot, slotBuf, sizeof(slotBuf)) == std::string_view(msg, n));
    }

    const bt_gatt_attr *isActiveAttr = nth_char_value(svc, 3 + TextAnimation::kNumStringSlots);
    zassert_not_null(isActiveAttr);
    zassert_equal(read_u8(isActiveAttr), 0);

    const bt_gatt_attr *nameAttr = nth_char_value(svc, 3 + TextAnimation::kNumStringSlots + 1);
    char nameBuf[32];
    zassert_true(read_str(nameAttr, nameBuf, sizeof(nameBuf)) == "Text");

    const bt_gatt_attr *nowPlayingAttr = nth_char_value(svc, 3 + TextAnimation::kNumStringSlots + 2);
    zassert_not_null(nowPlayingAttr);
    zassert_equal(read_u32(nowPlayingAttr), 0);

    text_animation_bind_default_dependencies();
    exercise_tick(TextAnimation::getInstance());
    AnimationIsActiveBinding<Animation::Text>::setLocalActiveState(true);
}

ZTEST(animation_adapters, test_my_eyes) {
    const bt_uuid_128 svcUuid =
        BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::MyEyes));
    const bt_gatt_service_static *svc = find_service(svcUuid);
    zassert_not_null(svc);

    zassert_equal(read_u32(nth_char_value(svc, 0)), 100);         // blink_speed_ms
    zassert_equal(read_u32(nth_char_value(svc, 1)), 0xFFFFFFFFu); // color
    zassert_equal(read_u32(nth_char_value(svc, 2)), 0);           // up_next

    char slotBuf[MyEyesAnimation::kMaxEyeLen];
    const bt_gatt_attr *slot0 = nth_char_value(svc, 3);
    zassert_not_null(slot0);
    zassert_true(read_str(slot0, slotBuf, sizeof(slotBuf)) == "^^");
    const char newEye[] = "OO";
    zassert_equal(do_write(slot0, newEye, sizeof(newEye) - 1), sizeof(newEye) - 1);
    zassert_true(read_str(slot0, slotBuf, sizeof(slotBuf)) == "OO");

    /* Same rationale as test_text: getMyEyesSlot()/setMyEyesSlot() are
     * per-index switch statements, so round-trip every slot. */
    for (size_t i = 1; i < MyEyesAnimation::kNumStringSlots; i++) {
        const bt_gatt_attr *slot = nth_char_value(svc, 3 + i);
        zassert_not_null(slot, "missing my_eyes slot %zu", i);
        char eye[4];
        int n = snprintf(eye, sizeof(eye), "%02zu", i % 100);
        zassert_equal(do_write(slot, eye, n), n);
        zassert_true(read_str(slot, slotBuf, sizeof(slotBuf)) == std::string_view(eye, n));
    }

    const bt_gatt_attr *isActiveAttr = nth_char_value(svc, 3 + MyEyesAnimation::kNumStringSlots);
    zassert_not_null(isActiveAttr);
    zassert_equal(read_u8(isActiveAttr), 0);

    const bt_gatt_attr *nameAttr = nth_char_value(svc, 3 + MyEyesAnimation::kNumStringSlots + 1);
    char nameBuf[32];
    zassert_true(read_str(nameAttr, nameBuf, sizeof(nameBuf)) == "MyEyes");

    my_eyes_animation_bind_default_dependencies();
    exercise_tick(MyEyesAnimation::getInstance());
    AnimationIsActiveBinding<Animation::MyEyes>::setLocalActiveState(true);
}

/* glim_player is the most bespoke adapter: BtGattDropdownList characteristics
 * (not a scalar/BtGattPersistentCharacteristic) built from glim_registry
 * state. This test runs in two phases: first with an empty registry (the
 * real boot-time state before glim_registry::init() has scanned anything,
 * or when /NAND:/glim has no files), then again after mounting the
 * native_sim ram-disk and provisioning one real file, to also exercise the
 * "registry has files" branches (buildGlimSelectionValue's loop body,
 * GlimSelectionCharacteristic::onWrite's success path) that the empty-registry
 * phase structurally can't reach. */
ZTEST(animation_adapters, test_glim_player) {
    zassert_equal(glim_registry::count(), 0, "expected an empty registry on native_sim");

    const bt_uuid_128 svcUuid =
        BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::GlimPlayer));
    const bt_gatt_service_static *svc = find_service(svcUuid);
    zassert_not_null(svc);

    const bt_gatt_attr *selectionAttr = nth_char_value(svc, 0);
    zassert_not_null(selectionAttr);
    char buf[64];
    /* Default-constructed BtGattDropdownList: empty string. */
    zassert_true(read_str(selectionAttr, buf, sizeof(buf)) == "");

    const bt_gatt_attr *loopModeAttr = nth_char_value(svc, 1);
    zassert_not_null(loopModeAttr);
    zassert_true(read_str(loopModeAttr, buf, sizeof(buf)) == "");

    const bt_gatt_attr *isActiveAttr = nth_char_value(svc, 2);
    zassert_not_null(isActiveAttr);
    zassert_equal(read_u8(isActiveAttr), 0);

    const bt_gatt_attr *nameAttr = nth_char_value(svc, 3);
    zassert_true(read_str(nameAttr, buf, sizeof(buf)) == "Glim Player");

    /* With an empty registry, bind_default_bt_dependencies() leaves the
     * selection characteristic alone (its "if (glim_registry::count() > 0)"
     * guard) but unconditionally seeds the loop-mode dropdown to its
     * "LoopOne" default - covers that seeding path plus
     * GlimPlayerAnimation::getInstance()->setDependencies(). */
    glim_player_animation_bind_default_bt_dependencies();
    zassert_true(read_str(loopModeAttr, buf, sizeof(buf)) == "Loop One\nPlay All\nStop After One");
    zassert_true(read_str(selectionAttr, buf, sizeof(buf)) == "");

    /* GlimLoopModeCharacteristic::onWrite(): a valid mode name switches the
     * dropdown's selected-first ordering. */
    const char *playAll = "Play All";
    zassert_equal(do_write(loopModeAttr, playAll, strlen(playAll)), (ssize_t)strlen(playAll));
    zassert_true(read_str(loopModeAttr, buf, sizeof(buf)) == "Play All\nLoop One\nStop After One");
    /* An unrecognized mode name fails loopModeFromName() inside onWrite(), so
     * the characteristic is never reassigned to a rebuilt canonical value -
     * but the raw bytes land in storage_ unconditionally before onWrite()
     * even runs (bt_service_cpp.h's generic string _write() has no rollback
     * path - see its "Checked write hooks are not supported for
     * string-backed types" comment), so the readback is just an echo of
     * whatever was written, not the previous canonical value. */
    const char *bogus = "Not A Mode";
    zassert_equal(do_write(loopModeAttr, bogus, strlen(bogus)), (ssize_t)strlen(bogus));
    zassert_true(read_str(loopModeAttr, buf, sizeof(buf)) == "Not A Mode");

    /* GlimSelectionCharacteristic::onWrite(): with an empty registry,
     * findGlimIndexByName() can never succeed, so this exercises exactly
     * that "selection not found" branch (onWrite returns before rebuilding
     * the canonical value) - same raw-write-lands-first caveat as above, so
     * the readback still echoes what was written. */
    const char *someFile = "whatever.glim";
    zassert_equal(do_write(selectionAttr, someFile, strlen(someFile)),
                 (ssize_t)strlen(someFile));
    zassert_true(read_str(selectionAttr, buf, sizeof(buf)) == "whatever.glim");

    uint8_t one = 1;
    zassert_equal(do_write(isActiveAttr, &one, sizeof(one)), sizeof(one));
    zassert_equal(read_u8(isActiveAttr), 1);

    AnimationIsActiveBinding<Animation::GlimPlayer>::setLocalActiveState(true);

    /* Phase 2: provision one real file and re-drive the selection path. */
    provision_one_glim_file("test.glim");
    zassert_equal(glim_registry::count(), 1);
    const char *fileName = glim_registry::name(0);
    zassert_not_null(fileName);

    /* buildGlimSelectionValue(0)'s loop body: exactly one file, so the
     * canonical value is just that file's name with no "\nOther" entries. */
    zassert_equal(do_write(selectionAttr, fileName, strlen(fileName)),
                 (ssize_t)strlen(fileName));
    char expected[64];
    snprintf(expected, sizeof(expected), "%s", fileName);
    zassert_true(read_str(selectionAttr, buf, sizeof(buf)) == expected);

    /* With a non-empty registry, GlimPlayerAnimation::tick() no longer
     * early-returns on "no files" - it proceeds to read
     * ConcreteGlimSelectionSource::currentIndex() and open/decode the file
     * this suite just wrote (a real, valid 1x1 single-frame GLIM). */
    glim_player_animation_bind_default_bt_dependencies();
    exercise_tick(GlimPlayerAnimation::getInstance());

    /* Lets the persistent_value_store debounce timer
     * (CONFIG_APP_SETTINGS_SAVE_DEBOUNCE_MS) actually fire, driving
     * glimSelectionDoSave()/glimLoopModeDoSave() for real instead of only
     * ever seeing request_save() get called. */
    k_sleep(K_MSEC(CONFIG_APP_SETTINGS_SAVE_DEBOUNCE_MS + 100));
}
