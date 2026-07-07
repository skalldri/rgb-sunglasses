/*
 * extension_host.cpp — sandboxed LLEXT animation extension host (issue #85).
 *
 * Executes extension code exclusively on one dedicated K_USER thread confined
 * to a single shared memory domain, re-initialized per activation. The kernel
 * side (this file + the pattern controller proxy) does all privileged work:
 * filesystem loading, domain setup, input snapshots, and framebuffer
 * copy-out. Extension code can only touch its own llext-allocated regions
 * (TEXT/RODATA/DATA/BSS partitions, added by llext_add_domain()) plus
 * z_libc_partition (TLS pointer — see the CONFIG_USERSPACE notes in
 * fw/CLAUDE.md for why every user thread needs it). 5 partitions total;
 * hardware-verified to fit the nRF5340's MPU budget (8 hardware regions,
 * ~4-5 usable partitions per domain after Zephyr's fixed background
 * mappings).
 *
 * Lifecycle (load-on-activate): boot discovery loads each extension
 * TRANSIENTLY to validate + copy out its manifest, then unloads it. Only the
 * active extension is llext-resident; activation records a pending load that
 * the pattern-controller thread performs lazily on the first tick() —
 * keeping FAT I/O and relocation off the BLE RX / shell threads. The
 * sandbox thread is recreated on every activation and after every fault or
 * deadline overrun; the extension's init/tick function pointers travel as
 * thread arguments, so the user thread reads no kernel-side state. An MPU
 * fault inside the extension aborts only the sandbox thread (see the fatal
 * handler override below), which the in-flight tick observes as a deadline
 * overrun.
 */

#include <animations/animation_registry.h>
#include <extensions/extension_animation_proxy.h>
#include <extensions/extension_bt.h>
#include <extensions/extension_host.h>
#include <extensions/extension_manifest.h>
#include <extensions/extension_param_persistence.h>
#include <extensions/extension_registry.h>
#include <led_controller.h>
#include <pattern_controller.h>
#include <settings/persistent_value_registry.h>
#include <settings/persistent_value_store.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/llext/fs_loader.h>
#include <zephyr/llext/llext.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/libc-hooks.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(CONFIG_AUDIO)
#include <sound/audio_dsp.h>
/* The rgbx ABI freezes the audio shape; if the DSP ever changes its band or
 * bucket counts this must become a translation layer, not a memcpy. */
static_assert(RGBX_AUDIO_NUM_BANDS == AUDIO_NUM_BANDS,
              "rgbx ABI audio band count must match the audio DSP");
static_assert(RGBX_AUDIO_NUM_DISPLAY_BUCKETS == AUDIO_NUM_DISPLAY_BUCKETS,
              "rgbx ABI display bucket count must match the audio DSP");
#endif

LOG_MODULE_REGISTER(ext_host);

namespace extension_host {
namespace {

/* Number of physical buttons feeding rgbx_inputs.buttons_pressed: sw0-sw3
 * (Up/Left/Right/Down on proto0) + the wake button. Matches kMaxButtons in
 * button_animation_source.cpp. */
constexpr size_t kNumButtons = 5;

struct Slot {
    bool loaded = false;   // discovered, validated, and registered (NOT llext-resident)
    bool faulted = false;
    size_t fileIndex = 0;  // extension_registry index this slot was loaded from
    extension_manifest::Metadata meta = {};
    /* Authoritative parameter values (host-owned so BLE reads/writes work
     * while the extension is not resident). */
    uint32_t paramValues[RGBX_MAX_PARAMS] = {};
    char stringValues[RGBX_MAX_STRING_PARAMS][RGBX_PARAM_STRING_MAX] = {};
    /* "ext/<sanitized displayName>" — built once in scan_slot(), used as the
     * persistent_value_registry key for this slot's param persistence. */
    char settingsKey[extension_param_persistence::kKeyMaxLen] = {};
    /* True once THIS slot owns its settingsKey in the persistent_value_registry
     * (register_slot_persistence() succeeded). False if registration was skipped
     * (persistence disabled), rolled back, or refused with -EEXIST because another
     * slot's sanitized display name produced the same key — in which case this
     * slot must NOT mark-dirty or fault-write that key, or it would clobber the
     * slot that actually owns it. */
    bool persistRegistered = false;
    /* Caller-owned registry record for this slot's param persistence (the registry links
     * it by pointer; the slot outlives the registration - see persistent_value_registry.h). */
    PersistentValueRegistryEntry persistEntry = {};
    /* Tick-handshake profiling (cycles), reset on every activation. */
    uint32_t tickMinCyc = 0;
    uint32_t tickMaxCyc = 0;
    uint64_t tickSumCyc = 0;
    uint32_t tickCount = 0;
};

Slot sSlots[kMaxExtensions];
size_t sSlotCount = 0;

/* The one llext-resident extension (see file-top lifecycle comment). Only
 * touched under sHostLock. */
struct {
    struct llext *ext = nullptr;
    struct rgbx_inputs *inputs = nullptr;
    uint8_t *framebuffer = nullptr;
    void (*initFn)() = nullptr;
    void (*tickFn)() = nullptr;
} sResident;

K_THREAD_STACK_DEFINE(sSandboxStack, CONFIG_APP_EXT_HOST_STACK_SIZE);
struct k_thread sSandboxThread;
bool sSandboxAlive = false;
int sActiveSlot = -1;       // slot the pattern controller should tick (-1 none)
int sPendingLoadSlot = -1;  // slot awaiting its lazy first-tick load (-1 none)

/* One shared sandbox domain, re-initialized per activation. Safe because
 * k_mem_domain_init() fully resets the object and the sandbox thread is
 * always aborted (which unlinks it from the domain) before re-init — the
 * "abort before re-init" invariant every teardown path preserves. */
struct k_mem_domain sSandboxDomain;

/* Handshake: host gives sReqSem to request one tick; sandbox gives sDoneSem
 * when the tick (or init) finished. Max count 1 — the protocol is strictly
 * synchronous. */
K_SEM_DEFINE(sReqSem, 0, 1);
K_SEM_DEFINE(sDoneSem, 0, 1);

/* Serializes every entry point that touches the singleton sandbox state
 * (thread object, handshake semaphores, sActiveSlot/sPendingLoadSlot,
 * sResident) or slot param values. Needed because
 * pattern_controller_change_to_animation() runs the switch synchronously on
 * the CALLER's thread — activate()/deactivate() arrive on the BT RX thread
 * (Is Active GATT write) or the shell thread while tick() runs on the
 * pattern-controller thread. k_mutex is owner-recursive, so nested locking
 * within one entry point is safe. tick() holds the lock across the one-time
 * lazy load (FAT I/O + relocation, tens of ms) — a concurrent BLE write
 * blocks that long, once, at activation. */
K_MUTEX_DEFINE(sHostLock);

struct HostLockGuard {
    HostLockGuard() { k_mutex_lock(&sHostLock, K_FOREVER); }
    ~HostLockGuard() { k_mutex_unlock(&sHostLock); }
};

AnimationImuSource *sImuSource = nullptr;
AnimationAudioSource *sAudioSource = nullptr;
AnimationButtonSource *sButtonSource = nullptr;

/* Runs in user mode inside the active extension's domain. p1/p2 are the
 * extension's init/tick entry points, resolved kernel-side at load time;
 * p3 is the llext handle, needed to run the extension's init arrays. */
void sandbox_entry(void *p1, void *p2, void *p3) {
    auto initFn = reinterpret_cast<void (*)()>(p1);
    auto tickFn = reinterpret_cast<void (*)()>(p2);
    auto ext = static_cast<struct llext *>(p3);

    /* Run the extension's C++ static constructors (init arrays) inside the
     * sandbox — llext_bringup() fetches the function table through the
     * llext_get_fn_table syscall, so this works from user mode and the
     * constructors execute with sandbox privileges, never kernel ones.
     * Required by the rgbx C++ wrapper (its static Animation instance's
     * vtable pointer is set here); a no-op for plain-C extensions. */
    (void)llext_bringup(ext);

    initFn();
    k_sem_give(&sDoneSem);

    while (true) {
        k_sem_take(&sReqSem, K_FOREVER);
        tickFn();
        k_sem_give(&sDoneSem);
    }
}

void sandbox_stop() {
    if (sSandboxAlive) {
        k_thread_abort(&sSandboxThread);
        sSandboxAlive = false;
    }
}

/* Tears down the sandbox AND unloads the resident extension (heap frees
 * only — no FS I/O — so this is safe on the BLE RX thread too). */
void unload_resident() {
    sandbox_stop();
    if (sResident.ext != nullptr) {
        llext_unload(&sResident.ext);
    }
    sResident = {};
    sActiveSlot = -1;
    sPendingLoadSlot = -1;
}

/* Marks the active slot dead after a load failure / deadline overrun /
 * fault, tears the sandbox down, and unloads the extension so the pattern
 * controller can keep running (issue #85 recovery). The slot stays faulted —
 * activate() rejects it — until clearFault() (shell `ext select`) explicitly
 * resets it. Un-marking Is Active notifies the app so it disables the dead
 * animation's toggle; the proxy renders the fault screen until the user
 * switches away. */
/* Seeds a slot's param/string values from its manifest defaults. Shared by boot
 * discovery (scan_slot) and fault recovery (sandbox_fault) so the two can't
 * silently diverge if default handling ever changes. */
void reset_params_to_defaults(Slot &slot) {
    for (size_t p = 0; p < slot.meta.paramCount; p++) {
        slot.paramValues[p] = slot.meta.params[p].defaultValue;
    }
    memcpy(slot.stringValues, slot.meta.stringDefaults, sizeof(slot.stringValues));
}

/* @param resetParams true only for faults that occur AFTER params were delivered
 * to the extension (tick-time crashes) — a persisted value could be the cause, so
 * reset it. false for load/init-time failures, which the params can't have caused
 * (they're only copied into the extension's inputs at tick time). */
void sandbox_fault(Slot &slot, const char *what, bool resetParams) {
    LOG_ERR("extension '%s': %s — aborting sandbox (`ext select` to retry)",
            slot.meta.displayName, what);
    slot.faulted = true;
    unload_resident();
    animation_registry_set_is_active(animationId(static_cast<size_t>(&slot - sSlots)), false);

    if (!resetParams) {
        /* Load/init-time failure (llext_load I/O error, llext heap exhaustion,
         * domain/thread setup, rgbx_init deadline miss). Params were never handed
         * to the extension, so they can't have caused this — leave the user's
         * saved values (RAM AND flash) intact; a single transient activation
         * failure must not permanently wipe them. */
        return;
    }

    /* A tick-time crash: a persisted param value MAY be what caused it — reset to
     * manifest defaults immediately, in RAM AND on flash (synchronously, not via
     * the debounced flush, so the clear can't be lost to a power cycle landing
     * inside that debounce window), so neither an `ext select` retry nor a future
     * boot can reproduce the same crash from the same poisoned value. Only touch
     * flash if THIS slot owns its persistence key (see persistRegistered) — else
     * the write would land on another slot's key. */
    reset_params_to_defaults(slot);
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG) && slot.persistRegistered) {
        extension_param_persistence::Blob defaults;
        extension_param_persistence::fill_blob(defaults, slot.meta, slot.paramValues,
                                               slot.stringValues);
        persistent_value_store::save_value(slot.settingsKey, &defaults, sizeof(defaults));
    }
}

}  // namespace
}  // namespace extension_host

/* Sandbox fault containment (issue #85, hardware-root-caused via GDB+SWD):
 * Zephyr's default (weak) k_sys_fatal_error_handler halts the ENTIRE system
 * on any fault — z_fatal_error() only demotes a fault to a thread abort if
 * this handler RETURNS, which the default never does. Without this override,
 * an MPU fault inside a sandboxed extension (verified: PC inside the llext
 * heap, reason 19) parked the CPU in arch_system_halt() and took down the
 * whole firmware, defeating the sandbox.
 *
 * The override returns — allowing z_fatal_error() to abort just the
 * offending thread — if and only if the faulting thread is the extension
 * sandbox thread (which is never essential). Kernel panics and faults on any
 * other thread keep the stock halt-everything behavior, preserving today's
 * debugging workflow (GDB attach to the halted CPU) for real firmware bugs.
 * Runs in exception context: keep it minimal. */
extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    ARG_UNUSED(esf);
    if (reason != K_ERR_KERNEL_PANIC &&
        k_current_get() == &extension_host::sSandboxThread) {
        LOG_ERR("fault (reason %u) in extension sandbox — aborting only the sandbox thread",
                reason);
        return;
    }
    log_panic();
    LOG_ERR("Halting system (reason %u)", reason);
    k_fatal_halt(reason);
}

namespace extension_host {
namespace {

/* Bytes remaining from `ptr` to the end of the extension memory region that
 * contains it, or 0 if `ptr` lies in none of the extension's four MPU
 * partition regions (TEXT/DATA/RODATA/BSS — the memory the extension itself
 * owns and the only memory the kernel may dereference on its behalf). */
size_t ext_region_remaining(const struct llext *ext, const void *ptr) {
    const auto addr = reinterpret_cast<uintptr_t>(ptr);
    for (int r = LLEXT_MEM_TEXT; r < LLEXT_MEM_PARTITIONS; r++) {
        const auto base = reinterpret_cast<uintptr_t>(ext->mem[r]);
        if (ext->mem[r] != nullptr && addr >= base && addr - base < ext->mem_size[r]) {
            return ext->mem_size[r] - (addr - base);
        }
    }
    return 0;
}

bool in_ext_memory(const struct llext *ext, const void *ptr, size_t len) {
    return len > 0 && ext_region_remaining(ext, ptr) >= len;
}

/* extension_manifest::InRegionFn thunk (ctx = the llext handle). */
bool in_region_thunk(void *ctx, const void *ptr, size_t len) {
    return in_ext_memory(static_cast<const struct llext *>(ctx), ptr, len);
}

/* Resolved required exports of a loaded extension. */
struct Exports {
    const struct rgbx_manifest *manifest;
    struct rgbx_inputs *inputs;
    uint8_t *framebuffer;
    void (*initFn)();
    void (*tickFn)();
};

/* Resolves the five required symbols and bounds-checks the exported data
 * blocks (an export whose real object is smaller than the ABI-required size
 * would otherwise have the kernel touching memory past it). */
bool resolve_exports(struct llext *ext, const char *path, Exports &out) {
    /* llext_find_sym returns const void*; inputs/framebuffer really are
     * writable extension globals, hence the const_casts. */
    out.manifest = static_cast<const struct rgbx_manifest *>(
        llext_find_sym(&ext->exp_tab, RGBX_SYM_MANIFEST));
    out.inputs = static_cast<struct rgbx_inputs *>(
        const_cast<void *>(llext_find_sym(&ext->exp_tab, RGBX_SYM_INPUTS)));
    out.framebuffer = static_cast<uint8_t *>(
        const_cast<void *>(llext_find_sym(&ext->exp_tab, RGBX_SYM_FRAMEBUFFER)));
    out.initFn = reinterpret_cast<void (*)()>(llext_find_sym(&ext->exp_tab, RGBX_SYM_INIT));
    out.tickFn = reinterpret_cast<void (*)()>(llext_find_sym(&ext->exp_tab, RGBX_SYM_TICK));

    if (!out.manifest || !out.inputs || !out.framebuffer || !out.initFn || !out.tickFn) {
        LOG_ERR("%s: missing required rgbx exports", path);
        return false;
    }
    return true;
}

/* Validates a loaded extension's manifest and data exports against the
 * display config; fills `meta` with the copied-out metadata. */
bool validate_loaded(struct llext *ext, const char *path, const Exports &exports,
                     extension_manifest::Metadata &meta) {
    const LedConfig *cfg = get_current_led_config();
    const extension_manifest::Env env = {
        .expectedWidth = static_cast<uint32_t>(cfg->displayWidth),
        .expectedHeight = static_cast<uint32_t>(cfg->displayHeight),
        .inRegion = in_region_thunk,
        .ctx = ext,
    };
    const extension_manifest::Result res =
        extension_manifest::validate(exports.manifest, env, meta);
    if (res != extension_manifest::Result::Ok) {
        LOG_ERR("%s: manifest rejected: %s", path, extension_manifest::result_str(res));
        return false;
    }

    if (!in_ext_memory(ext, exports.inputs, sizeof(struct rgbx_inputs)) ||
        !in_ext_memory(ext, exports.framebuffer, (size_t)meta.width * meta.height * 3)) {
        LOG_ERR("%s: inputs/framebuffer exports too small or outside extension memory", path);
        return false;
    }
    return true;
}

/* persistent_value_registry's load/save callbacks for one extension's combined
 * param blob (extension_param_persistence::Blob). Registered from
 * register_slot_persistence() during init(). ext_params_do_load is the live load
 * path: init() calls settings_load_subtree("appcfg/ext") once, AFTER all
 * extension keys are registered, so the shared "appcfg" handler dispatches each
 * persisted blob here on top of the seeded defaults (the boot-time settings_load()
 * in bluetooth_init() ran before these keys existed, so it couldn't). */
void ext_params_do_load(void *target, const void *data, size_t len) {
    if (len != sizeof(extension_param_persistence::Blob)) {
        return;
    }
    auto *slot = static_cast<Slot *>(target);
    extension_param_persistence::Blob blob;
    memcpy(&blob, data, sizeof(blob));
    extension_param_persistence::apply_blob(blob, slot->meta, slot->paramValues, slot->stringValues);
}

void ext_params_do_save(void *target) {
    auto *slot = static_cast<Slot *>(target);
    extension_param_persistence::Blob blob;
    /* Runs on the settings-save workqueue; snapshot the arrays under sHostLock
     * so a concurrent setParamValue()/writeParamString() on the BT RX thread
     * can't tear the blob (half of a new string, or param N updated but N+1 not)
     * on its way to flash. */
    {
        HostLockGuard lock;
        extension_param_persistence::fill_blob(blob, slot->meta, slot->paramValues,
                                               slot->stringValues);
    }
    persistent_value_store::save_value(slot->settingsKey, &blob, sizeof(blob));
}

/* Registers slot `slotIndex`'s param blob with the persistent_value_registry.
 * Called from init() ONLY after the slot's BLE service has fully registered, so a
 * slot rolled back on a registration failure never leaves a dangling registry
 * entry aliasing its settingsKey (which the next extension scanned into the reused
 * slot would overwrite). Sets persistRegistered on success; on -EEXIST (two
 * extensions whose sanitized display names collide) leaves it false so this slot
 * won't clobber the key the other slot owns. */
void register_slot_persistence(size_t slotIndex) {
    if (!IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        return;
    }
    Slot &slot = sSlots[slotIndex];
    /* Fill the caller-owned registry record (issue #114 — the registry links it by
     * pointer, and the slot outlives the registration) and register it. */
    slot.persistEntry.key = slot.settingsKey;
    slot.persistEntry.target = &slot;
    slot.persistEntry.load = ext_params_do_load;
    slot.persistEntry.save = ext_params_do_save;
    int ret = persistent_value_registry_register(&slot.persistEntry);
    if (ret == 0) {
        slot.persistRegistered = true;
    } else {
        LOG_WRN("extension '%s': param persistence unavailable (%d) - values won't survive reboot",
                slot.meta.displayName, ret);
    }
}

/* Boot-time discovery of registry entry `fileIndex` into slot `slotIndex`:
 * loads the ELF transiently, validates it, copies the metadata out, seeds
 * the parameter values, and unloads again (load-on-activate lifecycle). The
 * two indices diverge as soon as one file fails validation and is skipped,
 * so the slot records its file index for diagnostics (`ext list`). */
bool scan_slot(size_t fileIndex, size_t slotIndex) {
    Slot &slot = sSlots[slotIndex];

    char path[64];
    if (!extension_registry::full_path(fileIndex, path, sizeof(path))) {
        return false;
    }

    struct llext_fs_loader fs_loader = LLEXT_FS_LOADER(path);
    struct llext_load_param ldr_parm = LLEXT_LOAD_PARAM_DEFAULT;
    struct llext *ext = nullptr;

    int ret = llext_load(&fs_loader.loader, extension_registry::name(fileIndex), &ext, &ldr_parm);
    if (ret < 0) {
        LOG_ERR("llext_load(%s) failed: %d", path, ret);
        return false;
    }

    Exports exports;
    if (!resolve_exports(ext, path, exports) ||
        !validate_loaded(ext, path, exports, slot.meta)) {
        llext_unload(&ext);
        return false;
    }

    slot.fileIndex = fileIndex;
    reset_params_to_defaults(slot);

    /* Build the stable persistence key now, from the just-validated manifest
     * (keyed by the extension's display name, not slot index — see
     * extension_param_persistence.h). The registry registration and the one-shot
     * load of any persisted values are deferred to init(): registration happens
     * only AFTER this slot's BLE service fully registers (so a rolled-back slot
     * leaves no dangling registry entry), and the load is a single
     * settings_load_subtree() after the whole discovery loop rather than one
     * flash scan per slot here. */
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        extension_param_persistence::build_settings_key(slot.settingsKey, sizeof(slot.settingsKey),
                                                        slot.meta.displayName);
    }

    const size_t heapBytes = ext->alloc_size;
    llext_unload(&ext);

    slot.loaded = true;
    LOG_INF("discovered extension '%s' from %s (%zu bytes heap while loaded, %zu params)",
            slot.meta.displayName, path, heapBytes, slot.meta.paramCount);
    return true;
}

/* Performs the deferred activation load on the pattern-controller thread
 * (kernel mode — fs_* is legal here): loads the ELF, re-validates it,
 * cross-checks against the boot-time metadata, builds the shared domain, and
 * starts the sandbox thread through its rgbx_init() deadline. Returns false
 * with everything torn down on any failure. */
bool runtime_load(size_t slotIndex) {
    Slot &slot = sSlots[slotIndex];

    char path[64];
    if (!extension_registry::full_path(slot.fileIndex, path, sizeof(path))) {
        return false;
    }

    struct llext_fs_loader fs_loader = LLEXT_FS_LOADER(path);
    struct llext_load_param ldr_parm = LLEXT_LOAD_PARAM_DEFAULT;
    struct llext *ext = nullptr;

    int ret =
        llext_load(&fs_loader.loader, extension_registry::name(slot.fileIndex), &ext, &ldr_parm);
    if (ret < 0) {
        LOG_ERR("llext_load(%s) failed: %d", path, ret);
        return false;
    }

    /* Re-validate from scratch and cross-check against what boot discovery
     * saw: FAT can't change under the firmware without a reboot, but the
     * checks are cheap and a mismatch would otherwise hand the app stale
     * GATT metadata for a different param table. */
    Exports exports;
    extension_manifest::Metadata meta;
    bool ok = resolve_exports(ext, path, exports) && validate_loaded(ext, path, exports, meta);
    if (ok) {
        ok = strcmp(meta.displayName, slot.meta.displayName) == 0 &&
             meta.paramCount == slot.meta.paramCount &&
             meta.stringParamCount == slot.meta.stringParamCount;
        for (size_t p = 0; ok && p < meta.paramCount; p++) {
            ok = meta.params[p].type == slot.meta.params[p].type;
        }
        if (!ok) {
            LOG_ERR("%s: extension changed since boot discovery", path);
        }
    }
    if (!ok) {
        llext_unload(&ext);
        return false;
    }

    /* Domain = z_libc_partition + the extension's 4 llext partitions
     * (re-initializing the shared domain object is safe — see its
     * declaration). */
    struct k_mem_partition *parts[] = {&z_libc_partition};
    ret = k_mem_domain_init(&sSandboxDomain, ARRAY_SIZE(parts), parts);
    if (ret != 0) {
        LOG_ERR("%s: k_mem_domain_init failed: %d", path, ret);
        llext_unload(&ext);
        return false;
    }
    ret = llext_add_domain(ext, &sSandboxDomain);
    if (ret != 0) {
        LOG_ERR("%s: llext_add_domain failed: %d", path, ret);
        llext_unload(&ext);
        return false;
    }

    k_sem_reset(&sReqSem);
    k_sem_reset(&sDoneSem);

    k_tid_t tid = k_thread_create(&sSandboxThread, sSandboxStack,
                                  K_THREAD_STACK_SIZEOF(sSandboxStack), sandbox_entry,
                                  reinterpret_cast<void *>(exports.initFn),
                                  reinterpret_cast<void *>(exports.tickFn), ext,
                                  CONFIG_APP_EXT_HOST_THREAD_PRIORITY, K_FP_REGS | K_USER,
                                  K_FOREVER);
    k_thread_name_set(tid, "ext_sandbox");
    k_thread_access_grant(tid, &sReqSem, &sDoneSem);
    ret = k_mem_domain_add_thread(&sSandboxDomain, tid);
    if (ret != 0) {
        LOG_ERR("k_mem_domain_add_thread failed: %d", ret);
        k_thread_abort(tid);
        llext_unload(&ext);
        return false;
    }
    k_thread_start(tid);
    sSandboxAlive = true;

    sResident.ext = ext;
    sResident.inputs = exports.inputs;
    sResident.framebuffer = exports.framebuffer;
    sResident.initFn = exports.initFn;
    sResident.tickFn = exports.tickFn;

    /* Wait for the extension's rgbx_init() to finish (same deadline as a
     * tick — init runs sandboxed too, and a hang there must not stall the
     * pattern controller forever). */
    if (k_sem_take(&sDoneSem, K_MSEC(CONFIG_APP_EXT_TICK_DEADLINE_MS)) != 0) {
        LOG_ERR("%s: rgbx_init() missed deadline", path);
        unload_resident();
        return false;
    }

    slot.tickMinCyc = 0;
    slot.tickMaxCyc = 0;
    slot.tickSumCyc = 0;
    slot.tickCount = 0;

    LOG_INF("extension '%s' loaded and activated (%zu bytes heap)", slot.meta.displayName,
            ext->alloc_size);
    return true;
}

}  // namespace

void init() {
    extension_registry::init();

    for (size_t i = 0; i < extension_registry::count() && sSlotCount < kMaxExtensions; i++) {
        if (!scan_slot(i, sSlotCount)) {
            continue;
        }
        /* Count the slot first (the register functions validate against
         * isLoaded/count), then roll back on registration failure: the BLE
         * service registers before the proxy because it is the only one of
         * the two that can be unregistered. */
        const size_t slot = sSlotCount++;
        int ret = extension_bt_register(slot);
        if (ret != 0) {
            LOG_ERR("slot %zu: BLE service registration failed: %d", slot, ret);
            sSlots[slot].loaded = false;
            sSlotCount--;
            continue;
        }
        ret = extension_animation_proxy_register(slot);
        if (ret != 0) {
            LOG_ERR("slot %zu: animation registry registration failed: %d", slot, ret);
            extension_bt_unregister(slot);
            sSlots[slot].loaded = false;
            sSlotCount--;
            continue;
        }
        /* Last, because the registry only accepts setters for ids the proxy
         * registration just created. Failure would leave Is Active
         * reads/notifies dead for this slot, so treat it like the other
         * registration failures (the proxy entry can't be unregistered, but
         * an uncounted slot renders black and is invisible over BLE). */
        ret = extension_bt_bind_is_active(slot);
        if (ret != 0) {
            LOG_ERR("slot %zu: is-active binding failed: %d", slot, ret);
            extension_bt_unregister(slot);
            sSlots[slot].loaded = false;
            sSlotCount--;
            continue;
        }
        /* Fully registered — now (and only now) claim this slot's persistence
         * key. Doing it here rather than in scan_slot() means a slot rolled back
         * above never left a stale registry entry aliasing its settingsKey. */
        register_slot_persistence(slot);
    }

    /* One settings scan for every extension key, AFTER all are registered: the
     * shared "appcfg" handler -> registry dispatch -> ext_params_do_load chain
     * applies each persisted blob on top of its seeded defaults. Replaces the
     * former per-slot settings_load_one() (one full flash scan each on the NVS/ZMS
     * backend). The boot-time settings_load() in bluetooth_init() ran before these
     * keys existed, so this second, subtree-scoped pass is what actually loads
     * them; it touches only the appcfg/ext subtree, never other appcfg keys. */
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG) && sSlotCount > 0) {
        char subtree[16];
        snprintf(subtree, sizeof(subtree), "%s/ext", persistent_value_store::kSubtreeName);
        settings_load_subtree(subtree);
    }

    if (sSlotCount > 0) {
        LOG_INF("%zu extension animation(s) registered (ids 0x%02x..0x%02x)", sSlotCount,
                (unsigned)kAnimationIdBase, (unsigned)(kAnimationIdBase + sSlotCount - 1));
    }
}

size_t count() {
    return sSlotCount;
}

bool isLoaded(size_t slot) {
    return slot < sSlotCount && sSlots[slot].loaded;
}

bool isFaulted(size_t slot) {
    return slot < sSlotCount && sSlots[slot].faulted;
}

const char *name(size_t slot) {
    return slot < sSlotCount ? sSlots[slot].meta.displayName : nullptr;
}

size_t paramCount(size_t slot) {
    return slot < sSlotCount ? sSlots[slot].meta.paramCount : 0;
}

const ParamInfo *paramInfo(size_t slot, size_t index) {
    if (slot >= sSlotCount || index >= sSlots[slot].meta.paramCount) {
        return nullptr;
    }
    return &sSlots[slot].meta.params[index];
}

uint32_t paramValue(size_t slot, size_t index) {
    if (slot >= sSlotCount || index >= sSlots[slot].meta.paramCount) {
        return 0;
    }
    return sSlots[slot].paramValues[index];
}

void setParamValue(size_t slot, size_t index, uint32_t value) {
    if (slot >= sSlotCount || index >= sSlots[slot].meta.paramCount) {
        return;
    }
    /* Serialized against tick()'s params snapshot so one frame can't observe
     * a mix of old and new values across params. */
    HostLockGuard lock;
    sSlots[slot].paramValues[index] = value;
    /* Only persist if this slot owns its key (see persistRegistered) - a slot
     * whose registration was refused (-EEXIST on a duplicate display name) would
     * otherwise mark the OTHER slot's entry dirty and never save its own. */
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG) && sSlots[slot].persistRegistered) {
        persistent_value_registry_mark_dirty(sSlots[slot].settingsKey);
        persistent_value_store::request_save();
    }
}

const char *paramString(size_t slot, size_t index) {
    const ParamInfo *info = paramInfo(slot, index);
    if (info == nullptr || info->type != RGBX_PARAM_STRING ||
        info->stringSlot >= RGBX_MAX_STRING_PARAMS) {
        return "";
    }
    return sSlots[slot].stringValues[info->stringSlot];
}

bool writeParamString(size_t slot, size_t index, size_t offset, const void *data, size_t len) {
    const ParamInfo *info = paramInfo(slot, index);
    if (info == nullptr || info->type != RGBX_PARAM_STRING ||
        info->stringSlot >= RGBX_MAX_STRING_PARAMS) {
        return false;
    }
    /* Mirror the built-in string characteristics: the write plus its forced
     * NUL terminator must fit the buffer. */
    if (offset + len >= RGBX_PARAM_STRING_MAX) {
        return false;
    }
    HostLockGuard lock;
    char *dst = sSlots[slot].stringValues[info->stringSlot];
    memcpy(dst + offset, data, len);
    dst[offset + len] = '\0';
    /* See setParamValue(): only the slot that owns the key may persist to it. */
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG) && sSlots[slot].persistRegistered) {
        persistent_value_registry_mark_dirty(sSlots[slot].settingsKey);
        persistent_value_store::request_save();
    }
    return true;
}

Animation animationId(size_t slot) {
    return static_cast<Animation>(kAnimationIdBase + slot);
}

void set_imu_source(AnimationImuSource *source) {
    sImuSource = source;
}

void set_audio_source(AnimationAudioSource *source) {
    sAudioSource = source;
}

void set_button_source(AnimationButtonSource *source) {
    sButtonSource = source;
}

bool activate(size_t slot) {
    if (!isLoaded(slot)) {
        return false;
    }
    HostLockGuard lock;
    Slot &s = sSlots[slot];

    /* A faulted extension stays dead until explicitly reset (clearFault via
     * shell `ext select`) — BLE re-activation is rejected and the app's
     * toggle is pushed back off by the proxy/extension_bt. */
    if (s.faulted) {
        LOG_WRN("extension '%s' is faulted — activation rejected (`ext select` to reset)",
                s.meta.displayName);
        return false;
    }

    /* Cheap host-side work only (this commonly runs on the BLE RX thread):
     * tear down whatever is resident and defer the FAT load + sandbox
     * bring-up to the pattern-controller thread's first tick(). */
    unload_resident();
    sActiveSlot = static_cast<int>(slot);
    sPendingLoadSlot = static_cast<int>(slot);
    LOG_INF("extension '%s' activation queued", s.meta.displayName);
    return true;
}

void deactivate(size_t slot) {
    HostLockGuard lock;
    if (sActiveSlot == static_cast<int>(slot)) {
        unload_resident();
    }
}

void clearFault(size_t slot) {
    if (slot >= sSlotCount) {
        return;
    }
    HostLockGuard lock;
    sSlots[slot].faulted = false;
}

bool tick(size_t slot, uint32_t dtMs, AnimationRenderer &renderer) {
    /* Held across the whole handshake (and the one-time lazy load): an
     * Is Active write on the BT RX thread must not abort/recreate the
     * sandbox thread while this tick is between the request and done
     * semaphores. */
    HostLockGuard lock;
    if (!isLoaded(slot) || sSlots[slot].faulted || sActiveSlot != static_cast<int>(slot)) {
        return false;
    }
    Slot &s = sSlots[slot];

    if (sPendingLoadSlot == static_cast<int>(slot)) {
        sPendingLoadSlot = -1;
        if (!runtime_load(slot)) {
            sandbox_fault(s, "activation load failed", /*resetParams=*/false);
            return false;
        }
    }
    if (sResident.ext == nullptr) {
        return false;
    }

    /* Input snapshot, written directly into the extension's exported input
     * block (kernel mode may access user memory). Absent sources read as
     * zeros per the ABI contract. */
    struct rgbx_inputs *in = sResident.inputs;
    in->dt_ms = dtMs;
    memcpy(in->params, s.paramValues, sizeof(in->params));
    memcpy(in->param_strings, s.stringValues, sizeof(in->param_strings));
    if (sImuSource != nullptr) {
        sImuSource->update();
        in->accel[0] = sImuSource->getAccelX();
        in->accel[1] = sImuSource->getAccelY();
        in->accel[2] = sImuSource->getAccelZ();
        in->gyro[0] = sImuSource->getGyroX();
        in->gyro[1] = sImuSource->getGyroY();
        in->gyro[2] = sImuSource->getGyroZ();
    } else {
        memset(in->accel, 0, sizeof(in->accel));
        memset(in->gyro, 0, sizeof(in->gyro));
    }
    if (sAudioSource != nullptr) {
        sAudioSource->update();
        for (size_t b = 0; b < RGBX_AUDIO_NUM_BANDS; b++) {
            in->audio_band_energy[b] = sAudioSource->getBandEnergy(b);
            in->audio_beat[b] = sAudioSource->isBeat(b) ? 1 : 0;
        }
        for (size_t i = 0; i < RGBX_AUDIO_NUM_DISPLAY_BUCKETS; i++) {
            in->audio_display_bucket[i] = sAudioSource->getDisplayBucketEnergy(i);
        }
    } else {
        memset(in->audio_band_energy, 0, sizeof(in->audio_band_energy));
        memset(in->audio_beat, 0, sizeof(in->audio_beat));
        memset(in->audio_display_bucket, 0, sizeof(in->audio_display_bucket));
    }
    in->buttons_pressed = 0;
    if (sButtonSource != nullptr) {
        sButtonSource->update();
        for (size_t id = 0; id < kNumButtons; id++) {
            if (sButtonSource->wasPressed(id)) {
                in->buttons_pressed |= (1u << id);
            }
        }
    }

    const uint32_t startCyc = k_cycle_get_32();
    k_sem_give(&sReqSem);
    if (k_sem_take(&sDoneSem, K_MSEC(CONFIG_APP_EXT_TICK_DEADLINE_MS)) != 0) {
        /* Deadline overrun — either the extension is spinning or it MPU-
         * faulted (Zephyr already aborted the thread in that case; aborting
         * again is harmless). */
        sandbox_fault(s, "tick missed deadline (hang or fault)", /*resetParams=*/true);
        return false;
    }
    const uint32_t tickCyc = k_cycle_get_32() - startCyc;
    if (s.tickCount == 0 || tickCyc < s.tickMinCyc) {
        s.tickMinCyc = tickCyc;
    }
    if (tickCyc > s.tickMaxCyc) {
        s.tickMaxCyc = tickCyc;
    }
    s.tickSumCyc += tickCyc;
    s.tickCount++;

    /* Copy the extension's finished frame out to the real renderer. */
    for (uint32_t y = 0; y < s.meta.height; y++) {
        for (uint32_t x = 0; x < s.meta.width; x++) {
            const uint8_t *px = &sResident.framebuffer[RGBX_PIXEL_INDEX(s.meta.width, x, y)];
            renderer.setPixel(x, y, px[0], px[1], px[2]);
        }
    }
    return true;
}

namespace {

/* --- debug shell ------------------------------------------------------- */

int cmd_ext_list(const struct shell *sh, size_t, char **) {
    if (sSlotCount == 0) {
        shell_print(sh, "no extensions loaded");
        return 0;
    }
    for (size_t i = 0; i < sSlotCount; i++) {
        shell_print(sh, "[%zu] id=0x%02x '%s' file=%s params=%zu%s%s", i,
                    (unsigned)(kAnimationIdBase + i), sSlots[i].meta.displayName,
                    extension_registry::name(sSlots[i].fileIndex), sSlots[i].meta.paramCount,
                    sActiveSlot == (int)i ? " [active]" : "",
                    sSlots[i].faulted ? " [FAULTED]" : "");
    }
    return 0;
}

int cmd_ext_stats(const struct shell *sh, size_t, char **) {
    for (size_t i = 0; i < sSlotCount; i++) {
        const Slot &s = sSlots[i];
        if (s.tickCount == 0) {
            shell_print(sh, "[%zu] '%s': no ticks recorded", i, s.meta.displayName);
            continue;
        }
        shell_print(sh, "[%zu] '%s': %u ticks, handshake min/avg/max = %u/%u/%u us", i,
                    s.meta.displayName, s.tickCount, k_cyc_to_us_near32(s.tickMinCyc),
                    k_cyc_to_us_near32((uint32_t)(s.tickSumCyc / s.tickCount)),
                    k_cyc_to_us_near32(s.tickMaxCyc));
    }
    return 0;
}

int cmd_ext_param(const struct shell *sh, size_t argc, char **argv) {
    if (argc != 3 && argc != 4) {
        shell_error(sh, "Usage: ext param <slot> <index> [<value>]");
        return -EINVAL;
    }
    size_t slot = strtoul(argv[1], nullptr, 10);
    size_t index = strtoul(argv[2], nullptr, 10);
    const ParamInfo *info = paramInfo(slot, index);
    if (info == nullptr) {
        shell_error(sh, "no such param");
        return -ENOENT;
    }

    if (info->type == RGBX_PARAM_STRING) {
        if (argc == 4) {
            if (!writeParamString(slot, index, 0, argv[3], strlen(argv[3]))) {
                shell_error(sh, "string too long (max %u bytes)", RGBX_PARAM_STRING_MAX - 1);
                return -EINVAL;
            }
        }
        shell_print(sh, "%s.%s = \"%s\"", sSlots[slot].meta.displayName, info->name,
                    paramString(slot, index));
        return 0;
    }

    if (argc == 4) {
        uint32_t value = strtoul(argv[3], nullptr, 0);
        if (info->type == RGBX_PARAM_BOOL) {
            value = value ? 1 : 0;
        }
        setParamValue(slot, index, value);
    }
    uint32_t value = paramValue(slot, index);
    switch (info->type) {
        case RGBX_PARAM_BOOL:
            shell_print(sh, "%s.%s = %u", sSlots[slot].meta.displayName, info->name, value);
            break;
        case RGBX_PARAM_COLOR:
            shell_print(sh, "%s.%s = 0x%06x", sSlots[slot].meta.displayName, info->name,
                        value & 0x00FFFFFF);
            break;
        default:
            shell_print(sh, "%s.%s = %u (0x%x)", sSlots[slot].meta.displayName, info->name,
                        value, value);
            break;
    }
    return 0;
}

int cmd_ext_select(const struct shell *sh, size_t argc, char **argv) {
    if (argc != 2) {
        shell_error(sh, "Usage: ext select <slot>");
        return -EINVAL;
    }
    size_t slot = strtoul(argv[1], nullptr, 10);
    if (!isLoaded(slot)) {
        shell_error(sh, "no extension in slot %zu", slot);
        return -ENOENT;
    }
    /* The shell is the deliberate developer retry path for a dead extension:
     * clear the fault so activate() accepts it (BLE activation never does). */
    clearFault(slot);
    int ret = pattern_controller_change_to_animation(animationId(slot));
    if (ret == 0) {
        shell_print(sh, "switched to extension '%s'", sSlots[slot].meta.displayName);
    } else {
        shell_error(sh, "switch failed: %d", ret);
    }
    return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_ext, SHELL_CMD(list, NULL, "List loaded animation extensions", cmd_ext_list),
    SHELL_CMD(stats, NULL, "Per-extension tick-handshake timing (min/avg/max us)",
              cmd_ext_stats),
    SHELL_CMD_ARG(param, NULL, "Get/set a param: ext param <slot> <index> [<value>]",
                  cmd_ext_param, 3, 1),
    SHELL_CMD_ARG(select, NULL,
                  "Activate extension animation (clears a fault): ext select <slot>",
                  cmd_ext_select, 2, 0),
    SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(ext, &sub_ext, "Animation extension host", NULL);

}  // namespace
}  // namespace extension_host
