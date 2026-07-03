/*
 * extension_host.cpp — sandboxed LLEXT animation extension host (issue #85).
 *
 * Executes extension code exclusively on one dedicated K_USER thread confined
 * to the active extension's memory domain. The kernel side (this file + the
 * pattern controller proxy) does all privileged work: filesystem loading,
 * domain setup, input snapshots, and framebuffer copy-out. Extension code can
 * only touch its own llext-allocated regions (TEXT/RODATA/DATA/BSS
 * partitions, added by llext_add_domain()) plus z_libc_partition (TLS
 * pointer — see the CONFIG_USERSPACE notes in fw/CLAUDE.md for why every
 * user thread needs it). 5 partitions total; hardware-verified to fit the
 * nRF5340's MPU budget (the issue #85 "MPU spike").
 *
 * Sandbox lifecycle: the thread is recreated on every activation and after
 * every fault/deadline overrun. The extension's init/tick function pointers
 * travel as thread arguments, so the user thread reads no kernel-side state;
 * the two handshake semaphores are kernel objects reached via syscall and
 * granted at each creation. An MPU fault inside the extension aborts only
 * the sandbox thread (Zephyr's fatal handler; the thread is not essential),
 * which the next tick observes as a deadline overrun.
 */

#include <animations/animation_registry.h>
#include <extensions/extension_animation_proxy.h>
#include <extensions/extension_bt.h>
#include <extensions/extension_host.h>
#include <extensions/extension_registry.h>
#include <led_controller.h>
#include <pattern_controller.h>
#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/llext/fs_loader.h>
#include <zephyr/llext/llext.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/libc-hooks.h>

#include <cstdlib>
#include <cstring>

LOG_MODULE_REGISTER(ext_host);

namespace extension_host {
namespace {

struct Slot {
    bool loaded = false;
    bool faulted = false;
    size_t fileIndex = 0;  // extension_registry index this slot was loaded from
    struct llext *ext = nullptr;
    struct k_mem_domain domain;
    char displayName[kMaxNameLen] = {};
    uint32_t width = 0;
    uint32_t height = 0;
    size_t nParams = 0;
    ParamInfo params[RGBX_MAX_PARAMS] = {};
    uint32_t paramValues[RGBX_MAX_PARAMS] = {};
    /* Addresses inside the extension's (user) memory, cached at load time.
     * Kernel mode may read/write them directly. */
    struct rgbx_inputs *inputs = nullptr;
    uint8_t *framebuffer = nullptr;
    void (*initFn)() = nullptr;
    void (*tickFn)() = nullptr;
};

Slot sSlots[kMaxExtensions];
size_t sSlotCount = 0;

K_THREAD_STACK_DEFINE(sSandboxStack, CONFIG_APP_EXT_HOST_STACK_SIZE);
struct k_thread sSandboxThread;
bool sSandboxAlive = false;
int sActiveSlot = -1;

/* Handshake: host gives sReqSem to request one tick; sandbox gives sDoneSem
 * when the tick (or init) finished. Max count 1 — the protocol is strictly
 * synchronous. */
K_SEM_DEFINE(sReqSem, 0, 1);
K_SEM_DEFINE(sDoneSem, 0, 1);

/* Serializes every entry point that touches the singleton sandbox state
 * (thread object, handshake semaphores, sActiveSlot) or slot param values.
 * Needed because pattern_controller_change_to_animation() runs the switch
 * synchronously on the CALLER's thread — activate()/deactivate() arrive on
 * the BT RX thread (Is Active GATT write) or the shell thread while tick()
 * runs on the pattern-controller thread (PR #89 review finding 1). k_mutex
 * is owner-recursive, so nested locking within one entry point is safe. */
K_MUTEX_DEFINE(sHostLock);

struct HostLockGuard {
    HostLockGuard() { k_mutex_lock(&sHostLock, K_FOREVER); }
    ~HostLockGuard() { k_mutex_unlock(&sHostLock); }
};

AnimationImuSource *sImuSource = nullptr;

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
    sActiveSlot = -1;
}

/* Marks the active slot dead after a deadline overrun / fault and tears the
 * sandbox down so the pattern controller can keep running (issue #85
 * recovery). The slot stays faulted — activate() rejects it — until
 * clearFault() (shell `ext select`) explicitly resets it. Un-marking
 * Is Active notifies the app so it disables the dead animation's toggle. */
void sandbox_fault(Slot &slot, const char *what) {
    LOG_ERR("extension '%s': %s — aborting sandbox (`ext select` to retry)", slot.displayName,
            what);
    slot.faulted = true;
    sandbox_stop();
    animation_registry_set_is_active(animationId(static_cast<size_t>(&slot - sSlots)), false);
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

/* NUL-terminated string fully inside extension memory (NUL included)? */
bool ext_string_ok(const struct llext *ext, const char *str) {
    const size_t cap = ext_region_remaining(ext, str);
    return cap > 0 && memchr(str, '\0', cap) != nullptr;
}

/* Loads registry entry `fileIndex` into slot `slotIndex`. The two diverge as
 * soon as one file fails validation and is skipped, so the slot records its
 * file index for diagnostics (`ext list`). */
bool load_slot(size_t fileIndex, size_t slotIndex) {
    Slot &slot = sSlots[slotIndex];

    char path[64];
    if (!extension_registry::full_path(fileIndex, path, sizeof(path))) {
        return false;
    }

    struct llext_fs_loader fs_loader = LLEXT_FS_LOADER(path);
    struct llext_load_param ldr_parm = LLEXT_LOAD_PARAM_DEFAULT;

    int ret =
        llext_load(&fs_loader.loader, extension_registry::name(fileIndex), &slot.ext, &ldr_parm);
    if (ret < 0) {
        LOG_ERR("llext_load(%s) failed: %d", path, ret);
        return false;
    }

    /* llext_find_sym returns const void*; inputs/framebuffer really are
     * writable extension globals, hence the const_casts. */
    const auto *manifest = static_cast<const struct rgbx_manifest *>(
        llext_find_sym(&slot.ext->exp_tab, RGBX_SYM_MANIFEST));
    slot.inputs = static_cast<struct rgbx_inputs *>(
        const_cast<void *>(llext_find_sym(&slot.ext->exp_tab, RGBX_SYM_INPUTS)));
    slot.framebuffer = static_cast<uint8_t *>(
        const_cast<void *>(llext_find_sym(&slot.ext->exp_tab, RGBX_SYM_FRAMEBUFFER)));
    slot.initFn =
        reinterpret_cast<void (*)()>(llext_find_sym(&slot.ext->exp_tab, RGBX_SYM_INIT));
    slot.tickFn =
        reinterpret_cast<void (*)()>(llext_find_sym(&slot.ext->exp_tab, RGBX_SYM_TICK));

    if (!manifest || !slot.inputs || !slot.framebuffer || !slot.initFn || !slot.tickFn) {
        LOG_ERR("%s: missing required rgbx exports", path);
        llext_unload(&slot.ext);
        return false;
    }

    /* Validate the manifest before trusting any of it. Its embedded pointers
     * (name, params, each param's name) are untrusted relocated values from
     * the ELF: a corrupt or hostile file can make them point anywhere, and a
     * kernel-mode deref of a wild pointer faults a NON-sandbox thread — a
     * whole-system halt (PR #89 review finding 2). Every pointer is
     * bounds-checked against the extension's own regions before the copy-out
     * below, and nothing kernel-side dereferences extension manifest memory
     * after load. */
    if (!in_ext_memory(slot.ext, manifest, sizeof(*manifest))) {
        LOG_ERR("%s: manifest outside extension memory", path);
        llext_unload(&slot.ext);
        return false;
    }
    const LedConfig *cfg = get_current_led_config();
    if (manifest->abi_version != RGBX_ABI_VERSION) {
        LOG_ERR("%s: ABI version %u != %u", path, manifest->abi_version, RGBX_ABI_VERSION);
        llext_unload(&slot.ext);
        return false;
    }
    if (manifest->width != cfg->displayWidth || manifest->height != cfg->displayHeight) {
        LOG_ERR("%s: framebuffer %ux%u != display %ux%u", path, manifest->width, manifest->height,
                (unsigned)cfg->displayWidth, (unsigned)cfg->displayHeight);
        llext_unload(&slot.ext);
        return false;
    }
    if (manifest->param_count > RGBX_MAX_PARAMS ||
        (manifest->param_count > 0 &&
         !in_ext_memory(slot.ext, manifest->params,
                        manifest->param_count * sizeof(struct rgbx_param_desc)))) {
        LOG_ERR("%s: bad param table (count %u)", path, manifest->param_count);
        llext_unload(&slot.ext);
        return false;
    }
    if (manifest->name != nullptr && !ext_string_ok(slot.ext, manifest->name)) {
        LOG_ERR("%s: manifest name outside extension memory", path);
        llext_unload(&slot.ext);
        return false;
    }
    /* The exported blocks the host reads/writes every tick must also be
     * fully inside extension memory — an export whose real object is smaller
     * than the ABI-required size would otherwise have the kernel touching
     * memory past it. */
    if (!in_ext_memory(slot.ext, slot.inputs, sizeof(struct rgbx_inputs)) ||
        !in_ext_memory(slot.ext, slot.framebuffer,
                       (size_t)manifest->width * manifest->height * 3)) {
        LOG_ERR("%s: inputs/framebuffer exports too small or outside extension memory", path);
        llext_unload(&slot.ext);
        return false;
    }

    slot.fileIndex = fileIndex;
    slot.width = manifest->width;
    slot.height = manifest->height;
    strncpy(slot.displayName, manifest->name ? manifest->name : "unnamed", kMaxNameLen - 1);
    slot.nParams = manifest->param_count;
    for (size_t p = 0; p < slot.nParams; p++) {
        const struct rgbx_param_desc &desc = manifest->params[p];
        if (desc.name != nullptr && !ext_string_ok(slot.ext, desc.name)) {
            LOG_ERR("%s: param %zu name outside extension memory", path, p);
            llext_unload(&slot.ext);
            return false;
        }
        strncpy(slot.params[p].name, desc.name ? desc.name : "param", kMaxParamNameLen - 1);
        slot.params[p].type = desc.type;
        slot.params[p].defaultValue = desc.default_value;
        slot.paramValues[p] = desc.default_value;
    }

    /* Domain = z_libc_partition + the extension's 4 llext partitions. */
    struct k_mem_partition *parts[] = {&z_libc_partition};
    ret = k_mem_domain_init(&slot.domain, ARRAY_SIZE(parts), parts);
    if (ret != 0) {
        LOG_ERR("%s: k_mem_domain_init failed: %d", path, ret);
        llext_unload(&slot.ext);
        return false;
    }
    ret = llext_add_domain(slot.ext, &slot.domain);
    if (ret != 0) {
        LOG_ERR("%s: llext_add_domain failed: %d", path, ret);
        llext_unload(&slot.ext);
        return false;
    }

    slot.loaded = true;
    LOG_INF("loaded extension '%s' from %s (%zu bytes heap, %zu params)", slot.displayName, path,
            slot.ext->alloc_size, slot.nParams);
    return true;
}

}  // namespace

void init() {
    extension_registry::init();

    for (size_t i = 0; i < extension_registry::count() && sSlotCount < kMaxExtensions; i++) {
        if (!load_slot(i, sSlotCount)) {
            continue;
        }
        size_t slot = sSlotCount++;
        extension_animation_proxy_register(slot);
        extension_bt_register(slot);
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
    return slot < sSlotCount ? sSlots[slot].displayName : nullptr;
}

size_t paramCount(size_t slot) {
    return slot < sSlotCount ? sSlots[slot].nParams : 0;
}

const ParamInfo *paramInfo(size_t slot, size_t index) {
    if (slot >= sSlotCount || index >= sSlots[slot].nParams) {
        return nullptr;
    }
    return &sSlots[slot].params[index];
}

uint32_t paramValue(size_t slot, size_t index) {
    if (slot >= sSlotCount || index >= sSlots[slot].nParams) {
        return 0;
    }
    return sSlots[slot].paramValues[index];
}

void setParamValue(size_t slot, size_t index, uint32_t value) {
    if (slot >= sSlotCount || index >= sSlots[slot].nParams) {
        return;
    }
    /* Serialized against tick()'s params snapshot so one frame can't observe
     * a mix of old and new values across params (finding 6). */
    HostLockGuard lock;
    sSlots[slot].paramValues[index] = value;
}

Animation animationId(size_t slot) {
    return static_cast<Animation>(kAnimationIdBase + slot);
}

void set_imu_source(AnimationImuSource *source) {
    sImuSource = source;
}

bool activate(size_t slot) {
    if (!isLoaded(slot)) {
        return false;
    }
    HostLockGuard lock;
    Slot &s = sSlots[slot];

    /* A faulted extension stays dead until explicitly reset (clearFault via
     * shell `ext select`) — BLE re-activation is rejected and the app's
     * toggle is pushed back off by the proxy/extension_bt (finding 3). */
    if (s.faulted) {
        LOG_WRN("extension '%s' is faulted — activation rejected (`ext select` to reset)",
                s.displayName);
        return false;
    }

    sandbox_stop();
    k_sem_reset(&sReqSem);
    k_sem_reset(&sDoneSem);

    k_tid_t tid = k_thread_create(&sSandboxThread, sSandboxStack,
                                  K_THREAD_STACK_SIZEOF(sSandboxStack), sandbox_entry,
                                  reinterpret_cast<void *>(s.initFn),
                                  reinterpret_cast<void *>(s.tickFn), s.ext,
                                  CONFIG_APP_EXT_HOST_THREAD_PRIORITY, K_FP_REGS | K_USER,
                                  K_FOREVER);
    k_thread_name_set(tid, "ext_sandbox");
    k_thread_access_grant(tid, &sReqSem, &sDoneSem);
    int ret = k_mem_domain_add_thread(&s.domain, tid);
    if (ret != 0) {
        LOG_ERR("k_mem_domain_add_thread failed: %d", ret);
        k_thread_abort(tid);
        return false;
    }
    k_thread_start(tid);
    sSandboxAlive = true;
    sActiveSlot = static_cast<int>(slot);

    /* Wait for the extension's rgbx_init() to finish (same deadline as a
     * tick — init runs sandboxed too, and a hang there must not stall the
     * pattern controller forever). */
    if (k_sem_take(&sDoneSem, K_MSEC(CONFIG_APP_EXT_TICK_DEADLINE_MS)) != 0) {
        sandbox_fault(s, "rgbx_init() missed deadline");
        return false;
    }
    LOG_INF("extension '%s' activated", s.displayName);
    return true;
}

void deactivate(size_t slot) {
    HostLockGuard lock;
    if (sActiveSlot == static_cast<int>(slot)) {
        sandbox_stop();
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
    /* Held across the whole handshake: an Is Active write on the BT RX
     * thread must not abort/recreate the sandbox thread while this tick is
     * between the request and done semaphores (finding 1). Worst-case hold
     * is the tick deadline, which bounds how long a concurrent activation
     * can block. */
    HostLockGuard lock;
    if (!isLoaded(slot) || sSlots[slot].faulted || sActiveSlot != static_cast<int>(slot)) {
        return false;
    }
    Slot &s = sSlots[slot];

    /* Input snapshot, written directly into the extension's exported input
     * block (kernel mode may access user memory). */
    s.inputs->dt_ms = dtMs;
    memcpy(s.inputs->params, s.paramValues, sizeof(s.inputs->params));
    if (sImuSource != nullptr) {
        sImuSource->update();
        s.inputs->accel[0] = sImuSource->getAccelX();
        s.inputs->accel[1] = sImuSource->getAccelY();
        s.inputs->accel[2] = sImuSource->getAccelZ();
        s.inputs->gyro[0] = sImuSource->getGyroX();
        s.inputs->gyro[1] = sImuSource->getGyroY();
        s.inputs->gyro[2] = sImuSource->getGyroZ();
    } else {
        memset(s.inputs->accel, 0, sizeof(s.inputs->accel));
        memset(s.inputs->gyro, 0, sizeof(s.inputs->gyro));
    }

    k_sem_give(&sReqSem);
    if (k_sem_take(&sDoneSem, K_MSEC(CONFIG_APP_EXT_TICK_DEADLINE_MS)) != 0) {
        /* Deadline overrun — either the extension is spinning or it MPU-
         * faulted (Zephyr already aborted the thread in that case; aborting
         * again is harmless). */
        sandbox_fault(s, "tick missed deadline (hang or fault)");
        return false;
    }

    /* Copy the extension's finished frame out to the real renderer. */
    for (uint32_t y = 0; y < s.height; y++) {
        for (uint32_t x = 0; x < s.width; x++) {
            const uint8_t *px = &s.framebuffer[RGBX_PIXEL_INDEX(s.width, x, y)];
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
                    (unsigned)(kAnimationIdBase + i), sSlots[i].displayName,
                    extension_registry::name(sSlots[i].fileIndex), sSlots[i].nParams,
                    sActiveSlot == (int)i ? " [active]" : "",
                    sSlots[i].faulted ? " [FAULTED]" : "");
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
    if (slot >= sSlotCount || index >= sSlots[slot].nParams) {
        shell_error(sh, "no such param");
        return -ENOENT;
    }
    if (argc == 4) {
        setParamValue(slot, index, strtoul(argv[3], nullptr, 0));
    }
    uint32_t value = paramValue(slot, index);
    shell_print(sh, "%s.%s = %u (0x%x)", sSlots[slot].displayName, sSlots[slot].params[index].name,
                value, value);
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
        shell_print(sh, "switched to extension '%s'", sSlots[slot].displayName);
    } else {
        shell_error(sh, "switch failed: %d", ret);
    }
    return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_ext, SHELL_CMD(list, NULL, "List loaded animation extensions", cmd_ext_list),
    SHELL_CMD_ARG(param, NULL, "Get/set a param: ext param <slot> <index> [<value>]",
                  cmd_ext_param, 3, 1),
    SHELL_CMD_ARG(select, NULL,
                  "Activate extension animation (clears a fault): ext select <slot>",
                  cmd_ext_select, 2, 0),
    SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(ext, &sub_ext, "Animation extension host", NULL);

}  // namespace
}  // namespace extension_host
