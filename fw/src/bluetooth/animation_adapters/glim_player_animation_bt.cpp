#include <animations/animation_is_active_binding.h>
#include <animations/glim_player_animation.h>
#include <bluetooth/animation_is_active_characteristic.h>
#include <bluetooth/bt_service_cpp.h>
#include <settings/persistent_value_registry.h>
#include <settings/persistent_value_store.h>
#include <storage/glim_registry.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util_macro.h>

#include <algorithm>
#include <cstring>

#if defined(CONFIG_SHELL)
#include <zephyr/shell/shell.h>
#endif

constexpr bt_uuid_128 kGlimPlayerConfigServiceUuid =
    BT_ANIMATION_SERVICE_UUID(static_cast<uint16_t>(Animation::GlimPlayer));

// Generous enough for a handful of GLIM files (kMaxNameLen=32 each); buildGlimSelectionValue()
// stops appending names once the buffer is full rather than overflowing it.
constexpr size_t kGlimSelectionMaxLen = 512;
constexpr size_t kGlimLoopModeMaxLen = 64;
constexpr size_t kInvalidGlimIndex = static_cast<size_t>(-1);

namespace {

// Appends "\n<name>" (or just "<name>" if first) to a BtGattDropdownList's backing buffer,
// stopping short of overflowing it rather than writing past the end.
template <size_t N>
void appendDropdownOption(BtGattDropdownList<N> &dest, size_t &pos, const char *name) {
    size_t len = strlen(name);
    size_t needed = len + (pos > 0 ? 1 : 0);
    if (pos + needed >= N) {
        return;  // Would overflow (no room for the trailing '\0' either); drop silently.
    }
    if (pos > 0) {
        dest.value[pos++] = '\n';
    }
    memcpy(dest.value.data() + pos, name, len);
    pos += len;
    dest.value[pos] = '\0';
}

// Builds the canonical "Selected\nOther\nOther2..." value: every file currently known to
// glim_registry, with `selectedIndex` listed first.
BtGattDropdownList<kGlimSelectionMaxLen> buildGlimSelectionValue(size_t selectedIndex) {
    BtGattDropdownList<kGlimSelectionMaxLen> result{};
    size_t pos = 0;
    size_t count = glim_registry::count();
    if (count == 0) {
        return result;
    }
    if (selectedIndex >= count) {
        selectedIndex = 0;
    }

    appendDropdownOption(result, pos, glim_registry::name(selectedIndex));
    for (size_t i = 0; i < count; i++) {
        if (i == selectedIndex) {
            continue;
        }
        appendDropdownOption(result, pos, glim_registry::name(i));
    }
    return result;
}

constexpr const char *kLoopModeNames[] = {"Loop One", "Play All", "Stop After One"};
constexpr GlimLoopMode kLoopModeValues[] = {GlimLoopMode::LoopOne, GlimLoopMode::PlayAll,
                                            GlimLoopMode::StopAfterOne};
constexpr size_t kNumLoopModes = 3;

const char *loopModeName(GlimLoopMode mode) {
    for (size_t i = 0; i < kNumLoopModes; i++) {
        if (kLoopModeValues[i] == mode) {
            return kLoopModeNames[i];
        }
    }
    return kLoopModeNames[0];
}

// Returns true and sets *outMode if `name` exactly matches one of the 3 fixed loop mode options.
bool loopModeFromName(const char *name, GlimLoopMode *outMode) {
    for (size_t i = 0; i < kNumLoopModes; i++) {
        if (strcmp(name, kLoopModeNames[i]) == 0) {
            *outMode = kLoopModeValues[i];
            return true;
        }
    }
    return false;
}

// A dropdown-list characteristic's stored value is the canonical "Selected\nOther\nOther2..."
// list, not the bare selected name - copies just the first "\n"-delimited token into out.
void firstDropdownToken(const char *canonicalValue, char *out, size_t outCap) {
    const char *newline = strchr(canonicalValue, '\n');
    size_t len = newline ? static_cast<size_t>(newline - canonicalValue) : strlen(canonicalValue);
    len = std::min(len, outCap - 1);
    memcpy(out, canonicalValue, len);
    out[len] = '\0';
}

BtGattDropdownList<kGlimLoopModeMaxLen> buildLoopModeValue(GlimLoopMode mode) {
    BtGattDropdownList<kGlimLoopModeMaxLen> result{};
    size_t pos = 0;
    appendDropdownOption(result, pos, loopModeName(mode));
    for (size_t i = 0; i < kNumLoopModes; i++) {
        if (kLoopModeValues[i] != mode) {
            appendDropdownOption(result, pos, kLoopModeNames[i]);
        }
    }
    return result;
}

size_t findGlimIndexByName(const char *name) {
    size_t count = glim_registry::count();
    for (size_t i = 0; i < count; i++) {
        const char *n = glim_registry::name(i);
        if (n && strcmp(n, name) == 0) {
            return i;
        }
    }
    return kInvalidGlimIndex;
}

}  // namespace

BtGattPrimaryService<kGlimPlayerConfigServiceUuid> glimPlayerPrimaryService;

class GlimSelectionCharacteristic
    : public BtGattAutoCharacteristicExt<GlimSelectionCharacteristic, "Glim Selection", true, false,
                                         BtGattDropdownList<kGlimSelectionMaxLen>,
                                         BtGattDropdownList<kGlimSelectionMaxLen>{}> {
   public:
    using Base = BtGattAutoCharacteristicExt<GlimSelectionCharacteristic, "Glim Selection", true,
                                             false, BtGattDropdownList<kGlimSelectionMaxLen>,
                                             BtGattDropdownList<kGlimSelectionMaxLen>{}>;
    using Base::operator=;

    // The remote client writes just the bare option text (no separators). Validate it against
    // the registry, then reassign storage to the canonical selected-first value, which both
    // serves the next read and fires the existing change-detecting notify.
    void onWrite(const BtGattDropdownList<kGlimSelectionMaxLen> &written) {
        size_t index = findGlimIndexByName(written.value.data());
        if (index == kInvalidGlimIndex) {
            return;
        }
        sSelectedIndex = index;
        this->operator=(buildGlimSelectionValue(index));
        if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
            persistent_value_store::request_save();
        }
    }

    static size_t sSelectedIndex;
};
size_t GlimSelectionCharacteristic::sSelectedIndex = 0;

GlimSelectionCharacteristic glimSelection;

class GlimLoopModeCharacteristic
    : public BtGattAutoCharacteristicExt<GlimLoopModeCharacteristic, "Loop Mode", false, false,
                                         BtGattDropdownList<kGlimLoopModeMaxLen>,
                                         BtGattDropdownList<kGlimLoopModeMaxLen>{}> {
   public:
    using Base = BtGattAutoCharacteristicExt<GlimLoopModeCharacteristic, "Loop Mode", false, false,
                                             BtGattDropdownList<kGlimLoopModeMaxLen>,
                                             BtGattDropdownList<kGlimLoopModeMaxLen>{}>;
    using Base::operator=;

    void onWrite(const BtGattDropdownList<kGlimLoopModeMaxLen> &written) {
        GlimLoopMode mode;
        if (!loopModeFromName(written.value.data(), &mode)) {
            return;
        }
        this->operator=(buildLoopModeValue(mode));
        if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
            persistent_value_store::request_save();
        }
    }
};
GlimLoopModeCharacteristic glimLoopMode;

namespace {

constexpr const char *kGlimSelectionKey = "glim_player/selected_name";
constexpr const char *kGlimLoopModeKey = "glim_player/loop_mode";

// glim_registry hasn't scanned /NAND:/glim yet when settings_load() runs (that happens in
// bluetooth_init, before pattern_controller_thread_func's glim_registry::init() call - see
// the SYS_INIT ordering notes in fw/CLAUDE.md), so a loaded selection can't be resolved to
// an index here. Stash the raw name and resolve it once the registry is populated, in
// glim_player_animation_bind_default_bt_dependencies() below.
char sLoadedGlimSelectionName[glim_registry::kMaxNameLen] = {};
bool sGlimSelectionWasLoaded = false;

GlimLoopMode sLoadedGlimLoopMode = GlimLoopMode::LoopOne;
bool sGlimLoopModeWasLoaded = false;

void glimSelectionDoLoad(void *, const void *data, size_t len) {
    size_t copyLen = std::min(len, sizeof(sLoadedGlimSelectionName) - 1);
    memcpy(sLoadedGlimSelectionName, data, copyLen);
    sLoadedGlimSelectionName[copyLen] = '\0';
    sGlimSelectionWasLoaded = true;
}

void glimSelectionDoSave(void *) {
    size_t index = GlimSelectionCharacteristic::sSelectedIndex;
    if (index >= glim_registry::count()) {
        return;
    }
    const char *name = glim_registry::name(index);
    persistent_value_store::save_value(kGlimSelectionKey, name, strlen(name) + 1);
}

void glimLoopModeDoLoad(void *, const void *data, size_t len) {
    char name[kGlimLoopModeMaxLen] = {};
    size_t copyLen = std::min(len, sizeof(name) - 1);
    memcpy(name, data, copyLen);
    name[copyLen] = '\0';

    GlimLoopMode mode;
    if (loopModeFromName(name, &mode)) {
        sLoadedGlimLoopMode = mode;
        sGlimLoopModeWasLoaded = true;
    }
}

void glimLoopModeDoSave(void *) {
    char selected[kGlimLoopModeMaxLen];
    firstDropdownToken(glimLoopMode.value().data(), selected, sizeof(selected));
    persistent_value_store::save_value(kGlimLoopModeKey, selected, strlen(selected) + 1);
}

struct GlimPersistenceRegistrar {
    GlimPersistenceRegistrar() {
        // Skipped entirely (doLoad/doSave become unreferenced and get linked out) when
        // CONFIG_APP_PERSIST_BT_CONFIG=n, e.g. on rgb_sunglasses_dk - see fw/Kconfig.
        if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
            int err = persistent_value_registry_register(kGlimSelectionKey, nullptr,
                                                         glimSelectionDoLoad, glimSelectionDoSave);
            if (err) {
                printk("Failed to register glim selection persistence (err: %d)\n", err);
            }
            err = persistent_value_registry_register(kGlimLoopModeKey, nullptr, glimLoopModeDoLoad,
                                                     glimLoopModeDoSave);
            if (err) {
                printk("Failed to register glim loop mode persistence (err: %d)\n", err);
            }
        }
    }
};
[[maybe_unused]] GlimPersistenceRegistrar sGlimPersistenceRegistrar;

}  // namespace

using GlimPlayerIsActiveCharacteristic = IsActiveCharacteristic<Animation::GlimPlayer>;
GlimPlayerIsActiveCharacteristic glimPlayerIsActive;

constexpr BtGattString<24> kGlimPlayerAnimationName = makeBtGattString<24>("Glim Player");
BtGattReadOnlyCharacteristic<kAnimationNameCharacteristicUuid, "Animation Name", BtGattString<24>,
                             kGlimPlayerAnimationName>
    glimPlayerAnimationName;

BtGattServer glimPlayerConfigServer(glimPlayerPrimaryService, glimSelection, glimLoopMode,
                                    glimPlayerIsActive, glimPlayerAnimationName);
BT_GATT_SERVER_REGISTER(glimPlayerConfigServerStatic, glimPlayerConfigServer);

namespace {

class ConcreteGlimSelectionSource : public GlimSelectionSource {
   public:
    size_t currentIndex() const override { return GlimSelectionCharacteristic::sSelectedIndex; }

    void setSelection(size_t index) override {
        size_t count = glim_registry::count();
        if (count == 0 || index >= count) {
            return;
        }
        GlimSelectionCharacteristic::sSelectedIndex = index;
        glimSelection = buildGlimSelectionValue(index);
        if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
            persistent_value_store::request_save();
        }
    }
};

class ConcreteGlimLoopModeSource : public GlimLoopModeSource {
   public:
    GlimLoopMode get() const override {
        // glimLoopMode's stored value is the full canonical "Selected\nOther\nOther2..." list
        // (e.g. "Loop One\nPlay All\nStop After One"), not the bare selected name - extract just
        // the first token before comparing, or this never matches and silently stays stuck on
        // the GlimLoopMode::LoopOne fallback below regardless of what's actually selected.
        char selected[kGlimLoopModeMaxLen];
        firstDropdownToken(glimLoopMode.value().data(), selected, sizeof(selected));

        GlimLoopMode mode;
        if (loopModeFromName(selected, &mode)) {
            return mode;
        }
        return GlimLoopMode::LoopOne;
    }
};

ConcreteGlimSelectionSource sDefaultGlimSelectionSource;
ConcreteGlimLoopModeSource sDefaultGlimLoopModeSource;
GlimPlayerAnimationDependencies sDefaultGlimPlayerDeps(sDefaultGlimSelectionSource,
                                                       sDefaultGlimLoopModeSource);

}  // namespace

using GlimPlayerAnimationIsActive = AnimationIsActiveBinding<Animation::GlimPlayer>;

static void glim_player_set_is_active(bool active) {
    glimPlayerIsActive.setActive(active);
}

struct GlimPlayerIsActiveBindingRegistrar {
    GlimPlayerIsActiveBindingRegistrar() {
        GlimPlayerAnimationIsActive::registerSetter(glim_player_set_is_active);
    }
};

[[maybe_unused]] GlimPlayerIsActiveBindingRegistrar sGlimPlayerIsActiveBindingRegistrar;

void glim_player_animation_bind_default_bt_dependencies() {
    // Seed the selection/loop-mode characteristics now that glim_registry has scanned
    // /NAND:/glim (glim_registry::init() runs earlier in pattern_controller_thread_func(),
    // before animation_registry_register_defaults() reaches this call). If a selection was
    // persisted, resolve its file name to an index now (this is the first point at which
    // glim_registry is actually populated) - fall back to index 0 if the named file is no
    // longer present.
    size_t initialIndex = 0;
    if (sGlimSelectionWasLoaded) {
        size_t found = findGlimIndexByName(sLoadedGlimSelectionName);
        if (found != kInvalidGlimIndex) {
            initialIndex = found;
        }
    }
    GlimSelectionCharacteristic::sSelectedIndex = initialIndex;
    if (glim_registry::count() > 0) {
        glimSelection = buildGlimSelectionValue(initialIndex);
    }

    glimLoopMode =
        buildLoopModeValue(sGlimLoopModeWasLoaded ? sLoadedGlimLoopMode : GlimLoopMode::LoopOne);

    GlimPlayerAnimation::getInstance()->setDependencies(sDefaultGlimPlayerDeps);
}

#if defined(CONFIG_SHELL)

static int cmd_glim_list(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    size_t count = glim_registry::count();
    if (count == 0) {
        shell_print(shell, "(no GLIM files found in %s)", glim_registry::kDirectory);
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        shell_print(shell, "%zu: %s%s", i, glim_registry::name(i),
                    i == GlimSelectionCharacteristic::sSelectedIndex ? " (selected)" : "");
    }
    return 0;
}

static int cmd_glim_select(const struct shell *shell, size_t argc, char **argv) {
    if (argc != 2) {
        shell_error(shell, "Usage: glim select <index>");
        return -EINVAL;
    }
    char *endptr = nullptr;
    long index = strtol(argv[1], &endptr, 10);
    if (endptr == argv[1] || index < 0) {
        shell_error(shell, "Invalid index: %s", argv[1]);
        return -EINVAL;
    }
    sDefaultGlimSelectionSource.setSelection(static_cast<size_t>(index));
    return 0;
}

static int cmd_glim_get_selected(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    size_t count = glim_registry::count();
    size_t selected = GlimSelectionCharacteristic::sSelectedIndex;
    if (count == 0 || selected >= count) {
        shell_print(shell, "(none)");
        return 0;
    }
    shell_print(shell, "%zu: %s", selected, glim_registry::name(selected));
    return 0;
}

static int cmd_glim_set_loop_mode(const struct shell *shell, size_t argc, char **argv) {
    if (argc != 2) {
        shell_error(shell, "Usage: glim set_loop_mode <loop_one|play_all|stop_after_one>");
        return -EINVAL;
    }

    GlimLoopMode mode;
    if (strcmp(argv[1], "loop_one") == 0) {
        mode = GlimLoopMode::LoopOne;
    } else if (strcmp(argv[1], "play_all") == 0) {
        mode = GlimLoopMode::PlayAll;
    } else if (strcmp(argv[1], "stop_after_one") == 0) {
        mode = GlimLoopMode::StopAfterOne;
    } else {
        shell_error(shell, "Unknown loop mode: %s", argv[1]);
        return -EINVAL;
    }

    glimLoopMode = buildLoopModeValue(mode);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        persistent_value_store::request_save();
    }
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_glim, SHELL_CMD(list, NULL, "List discovered GLIM files", cmd_glim_list),
    SHELL_CMD_ARG(select, NULL, "Select a GLIM file by index", cmd_glim_select, 2, 0),
    SHELL_CMD(get_selected, NULL, "Print the currently selected GLIM file", cmd_glim_get_selected),
    SHELL_CMD_ARG(set_loop_mode, NULL, "Set loop mode (loop_one|play_all|stop_after_one)",
                  cmd_glim_set_loop_mode, 2, 0),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(glim, &sub_glim, "Glim player commands", NULL);

#endif /* CONFIG_SHELL */
