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
#include <animations/color_mode_source.h>
#include <extensions/extension_animation_proxy.h>
#include <extensions/extension_bt.h>
#include <extensions/extension_host.h>
#include <extensions/extension_manifest.h>
#include <extensions/extension_param_persistence.h>
#include <extensions/extension_registry.h>
#include <extensions/extension_tick_budget.h>
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
#include <zephyr/random/random.h>
#include <zephyr/shell/shell.h>
#include <strings.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/libc-hooks.h>
#include <zephyr/sys/reboot.h>

#include <cmsis_core.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

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
    /* Retired by a runtime FILE_MGMT delete: the .llext is gone from disk, so
     * re-activation (which re-reads FAT — load-on-activate) can only fail.
     * Rejected by activate(), skipped by shuffle, NOT cleared by clearFault();
     * forgotten naturally at the next boot's rescan. */
    bool retired = false;
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
    /* "Include in shuffle" flag (issue #243): its own settings key beside the param
     * blob ("ext/<sanitized displayName>/shuffle") so sandbox_fault()'s param clear
     * never touches it — the flag can't have caused a crash. Same ownership rules as
     * settingsKey/persistRegistered above. */
    bool shuffleInclude = true;
    char shuffleKey[extension_param_persistence::kShuffleKeyMaxLen] = {};
    bool shufflePersistRegistered = false;
    PersistentValueRegistryEntry shufflePersistEntry = {};
    /* Caller-owned work records for purgePersistence()'s async cleanup (one
     * per registry entry above) — the purge outlives the DELETE handler's
     * stack frame, so the storage must live here, not there. */
    persistent_value_store::PersistentValuePurge persistPurge = {};
    persistent_value_store::PersistentValuePurge shufflePurge = {};
    /* Tick profiling (cycles), reset on every activation. Wall and CPU are
     * tracked SEPARATELY on purpose (issue #276): wall time includes every
     * higher-priority thread that preempted the sandbox mid-tick, so a single
     * conflated number reads as "the extension got slow" when in fact the
     * system got busy. `ext stats` prints both so the two are never confused
     * again. Only `tickWall.count` is used as the "has ticked" test; the two
     * accumulators are always recorded together. */
    TickStatAccumulator tickWall = {};
    TickStatAccumulator tickCpu = {};
    /* Shuffle's good-switch-point signal (issue #121), cached from the extension's
     * optional rgbx_good_moment export after each completed tick. Defaults to true
     * (and stays true when the extension doesn't export the symbol) so shuffle mode
     * treats it exactly like a built-in animation with no override. */
    bool goodMoment = true;
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
    /* Optional export — nullptr when the extension predates / doesn't use it. */
    uint8_t *goodMoment = nullptr;
} sResident;

K_THREAD_STACK_DEFINE(sSandboxStack, CONFIG_APP_EXT_HOST_STACK_SIZE);

// A runaway extension must always be preemptible by the pattern controller thread, which
// is what enforces its per-tick deadline. See fw/docs/threading.md.
BUILD_ASSERT(CONFIG_APP_EXT_HOST_THREAD_PRIORITY > CONFIG_APP_PATTERN_CONTROLLER_THREAD_PRIORITY,
             "the extension sandbox must rank below pattern_controller_thread");
BUILD_ASSERT(CONFIG_APP_EXT_HOST_THREAD_PRIORITY >= 0 &&
                 CONFIG_APP_EXT_HOST_THREAD_PRIORITY < CONFIG_NUM_PREEMPT_PRIORITIES,
             "CONFIG_APP_EXT_HOST_THREAD_PRIORITY must be a valid preemptible priority");
/* The wall backstop only makes sense as a ceiling ABOVE the CPU budget: a tick
 * cannot consume more CPU than wall time, so an inverted pair would make the
 * backstop fire first and reintroduce the issue #276 starvation false
 * positives that the CPU budget exists to eliminate. */
BUILD_ASSERT(CONFIG_APP_EXT_TICK_WALL_BACKSTOP_MS > CONFIG_APP_EXT_TICK_CPU_BUDGET_MS,
             "CONFIG_APP_EXT_TICK_WALL_BACKSTOP_MS must exceed the per-tick CPU budget");
/* APP_EXTENSION_HOST selects THREAD_RUNTIME_STATS, which enables this by
 * default — but a board .conf could still turn it off, and the failure mode
 * would be silent (execution_cycles absent, every tick billed as zero CPU, no
 * runaway ever detected). Fail the build instead. */
#if !defined(CONFIG_SCHED_THREAD_USAGE)
#error "extension_host requires CONFIG_SCHED_THREAD_USAGE for per-tick CPU accounting"
#endif
/* The budget is built in the system-clock domain (k_ms_to_cyc_ceil64(), 32768 Hz
 * on proto0) and compared against k_thread_runtime_stats_t::execution_cycles.
 * Those units agree only while Zephyr's usage accounting uses the same clock.
 * Setting THREAD_RUNTIME_STATS_USE_TIMING_FUNCTIONS switches it to
 * timing_counter_get() — the DWT counter at the 64/128 MHz CPU clock — which is
 * a ~2000x unit change in the WRONG direction: a healthy 4.7 ms tick would
 * report ~300000 cycles against a 50 ms budget of 1639, so evaluate_tick_budget()
 * would return CpuBudgetExceeded on the very first tick of every extension and
 * sandbox_fault() would wipe each slot's persisted params to flash. That symbol
 * is user-selectable under the THREAD_RUNTIME_STATS menu this file's Kconfig
 * force-selects, and enabling it is the obvious move when profiling — so guard
 * it rather than leaving a profiling session to destroy user settings. */
#if defined(CONFIG_THREAD_RUNTIME_STATS_USE_TIMING_FUNCTIONS)
#error \
    "extension_host's CPU budget assumes k_cycle_get_32() units; \
THREAD_RUNTIME_STATS_USE_TIMING_FUNCTIONS reports a different clock domain"
#endif
struct k_thread sSandboxThread;
bool sSandboxAlive = false;
int sActiveSlot = -1;       // slot the pattern controller should tick (-1 none)
int sPendingLoadSlot = -1;  // slot awaiting its lazy first-tick load (-1 none)

/* Color-mode resolution for extension COLOR params (issue #259): one resolver
 * per param INDEX, shared across slots — only the active slot ever ticks, and
 * activate() arms a state reset on every resolver, so cross-slot state can't
 * leak. The raw source reads the host-owned authoritative value (which keeps
 * carrying the mode byte through BLE reads and persistence); tick() copies the
 * RESOLVED effective color into rgbx_inputs, so extensions keep seeing a plain
 * 0x00RRGGBB through the unchanged ABI (rgbx paramColor() masks to 24 bits
 * anyway). */
class ActiveSlotParamSource : public AnimationUint32ParameterSource {
   public:
    constexpr explicit ActiveSlotParamSource(size_t index) : index_(index) {}
    uint32_t get() const override {
        return sActiveSlot >= 0 ? sSlots[sActiveSlot].paramValues[index_] : 0u;
    }

   private:
    size_t index_;
};

struct ParamColorResolver {
    explicit ParamColorResolver(size_t index)
        : raw(index), mode(raw, sys_rand32_get, k_uptime_get) {}
    ActiveSlotParamSource raw;
    ColorModeSource mode;  // references `raw` — member order matters
};

/* One resolver per param slot, built from an index sequence so a change to
 * RGBX_MAX_PARAMS scales automatically (same idiom as kFactories in
 * extension_animation_proxy.cpp) rather than needing a hand-written element
 * list. ColorModeSource is neither copyable nor movable (atomic member), so
 * this relies on C++17 guaranteed elision constructing each element in place
 * from its prvalue -- which is also why the elements are built here rather
 * than assigned in a loop. */
template <size_t... I>
std::array<ParamColorResolver, sizeof...(I)> make_param_color_resolvers(
    std::index_sequence<I...>) {
    return {{ParamColorResolver(I)...}};
}

std::array<ParamColorResolver, RGBX_MAX_PARAMS> sParamColorResolvers =
    make_param_color_resolvers(std::make_index_sequence<RGBX_MAX_PARAMS>{});

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

/* Poll granularity for the handshake wait, in ms. Small and deliberately
 * independent of the CPU budget: it sets how promptly a runaway is noticed once
 * it has actually exceeded its budget, and it is the amount by which the wall
 * deadline can be overshot (the deadline is only re-checked at poll
 * boundaries). The real ceiling on one tick() is therefore
 * CONFIG_APP_EXT_TICK_WALL_BACKSTOP_MS + kPollMs, which is what Kconfig and
 * fw/docs/threading.md state. Bounded by the CPU budget so a deliberately tiny
 * budget still gets at least one sample per period. */
constexpr int kPollMs = MAX(1, MIN(CONFIG_APP_EXT_TICK_CPU_BUDGET_MS, 10));

/** @brief CPU cycles the sandbox thread has consumed over its lifetime, or 0
 *  if the kernel can't report them (thread not yet running). Only ever used as
 *  a delta across one handshake, so the absolute origin does not matter. */
uint64_t sandbox_exec_cycles() {
    k_thread_runtime_stats_t stats;
    if (!sSandboxAlive || k_thread_runtime_stats_get(&sSandboxThread, &stats) != 0) {
        return 0;
    }
    return stats.execution_cycles;
}

/** @brief True once the sandbox thread has terminated — which for this thread
 *  means it took a CPU fault and k_sys_fatal_error_handler let Zephyr abort it
 *  (issue #85 containment). K_NO_WAIT makes this a state query, safe to call
 *  while holding sHostLock. */
bool sandbox_thread_terminated() {
    return sSandboxAlive && k_thread_join(&sSandboxThread, K_NO_WAIT) == 0;
}

/**
 * @brief Waits for one sandbox handshake, budgeting the sandbox's own CPU
 * time rather than elapsed wall time (issue #276).
 *
 * The CPU budget can only be sampled where this thread runs, i.e. at poll
 * boundaries, so the poll period sets the detection granularity for a runaway.
 * kPollMs is therefore kept small and independent of the budget: a healthy tick
 * still returns on the very first take (the semaphore is already given, so no
 * extra context switches), while a spinner is caught within about one period of
 * actually exceeding its budget instead of one whole budget later. Detection is
 * still load-dependent by construction — an extension granted 10% of the CPU
 * needs 10x the wall time to burn its budget — which is the correct behaviour
 * for a CPU budget, and the wall deadline bounds the tail.
 *
 * @param deadline   Shared wall-clock deadline. tick() computes ONE per call and
 *        passes the same timepoint to the lazy load's handshake and its own, so
 *        a load followed by a tick can never together exceed one backstop.
 * @param postRequest true to post the tick request AFTER the measurement
 *        origin is taken. The rgbx_init path passes false: the sandbox thread
 *        signals done on its own once init returns, with no request to post.
 * @param cpuCycOut  CPU cycles the sandbox consumed (valid on every return).
 * @param wallCycOut Wall cycles elapsed in THIS wait (valid on every return;
 *        for `ext stats` only — the deadline, not this, bounds the wait).
 * @return The terminal verdict — never TickVerdict::Running.
 */
TickVerdict wait_for_sandbox(k_timepoint_t deadline, bool postRequest, uint32_t &cpuCycOut,
                             uint32_t &wallCycOut) {
    /* Cycle-domain budget. Not constexpr: sys_clock_hw_cycles_per_sec() is a
     * runtime call on targets that read their timer frequency at boot, so the
     * conversion has to go through Zephyr's own helper. */
    const TickBudgetLimits limits{k_ms_to_cyc_ceil64(CONFIG_APP_EXT_TICK_CPU_BUDGET_MS)};

    /* Origin BEFORE the request is posted. Taking it after would let a
     * preemption of THIS thread (pattern_controller, priority 4, is itself
     * preemptible by led_display at 2) slide the origin later and silently
     * loosen the reported wall figures. */
    const uint32_t startCyc = k_cycle_get_32();
    const uint64_t startExec = sandbox_exec_cycles();
    if (postRequest) {
        k_sem_give(&sReqSem);
    }

    TickBudgetSample sample;
    TickVerdict verdict;
    do {
        sample.completed = (k_sem_take(&sDoneSem, K_MSEC(kPollMs)) == 0);
        /* Defensive clamp. Not because the kernel ever reports zero for a live
         * thread — k_thread_runtime_stats_get() reads thread->base.usage.total
         * and an aborted sandbox keeps reporting its final accumulated total —
         * but because the two samples are taken at different times and a
         * wrapped/reset accumulator must not turn into a fake CPU overrun. */
        const uint64_t execNow = sandbox_exec_cycles();
        sample.cpuCyc = execNow > startExec ? execNow - startExec : 0;
        sample.sandboxDied = sandbox_thread_terminated();
        sample.wallDeadlineExpired = sys_timepoint_expired(deadline);
        verdict = evaluate_tick_budget(sample, limits);
    } while (verdict == TickVerdict::Running);

    cpuCycOut = (uint32_t)sample.cpuCyc;
    wallCycOut = k_cycle_get_32() - startCyc;
    return verdict;
}

/** @brief Human-readable cause for a fault verdict, for the log line. */
const char *verdict_describe(TickVerdict verdict) {
    switch (verdict) {
        case TickVerdict::CpuBudgetExceeded:
            return "tick exceeded its CPU budget (runaway extension)";
        case TickVerdict::SandboxDied:
            return "sandbox thread died mid-tick (CPU fault inside the extension)";
        case TickVerdict::WallBackstopExceeded:
            return "tick never completed before the wall-clock backstop (blocked, or "
                   "the system is severely overloaded)";
        default:
            return "tick failed";
    }
}

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

/*
 * Latched fault records (issue #308). One per slot, holding the MOST RECENT fault plus a
 * count of how many times that slot has faulted.
 *
 * This exists because the fault reason was previously a single LOG_ERR and nothing else.
 * That line scrolls out of the UART backlog quickly, and on this board the log is not a
 * reliable delivery mechanism at all — the USB CDC backend only appears seconds into
 * boot, after CONFIG_LOG_BUFFER_SIZE has overflowed, so a fault during boot-time
 * extension discovery may never reach a console. A transient fault was therefore
 * effectively undiagnosable after the fact, which is what issue #308 was reduced to.
 *
 * Latched until explicitly cleared rather than until the next fault, so an operator can
 * establish a clean baseline (`ext faults clear`), reproduce, and read exactly what
 * happened.
 *
 * `what` stores the POINTER, not a copy: every call site passes either a string literal
 * or verdict_describe()'s static strings, both of which have static storage duration.
 * Do not pass a stack buffer. The display name IS copied, because a slot's metadata is
 * rewritten by re-discovery and the record has to outlive that.
 */
struct FaultRecord {
    bool valid;
    char name[kMaxNameLen];
    const char *what;
    int64_t uptimeMs;
    uint32_t cpuUs;   /* 0 for load/init failures — no tick was measured */
    uint32_t wallUs;  /* 0 for load/init failures */
    uint32_t count;   /* faults on this slot since the last clear */
    bool paramsReset;
};

FaultRecord sFaults[kMaxExtensions];

/* @param resetParams true only for faults that occur AFTER params were delivered
 * to the extension (tick-time crashes) — a persisted value could be the cause, so
 * reset it. false for load/init-time failures, which the params can't have caused
 * (they're only copied into the extension's inputs at tick time). */
void sandbox_fault(Slot &slot, const char *what, bool resetParams, uint32_t cpuUs = 0,
                  uint32_t wallUs = 0) {
    LOG_ERR("extension '%s': %s — aborting sandbox (`ext select` to retry)",
            slot.meta.displayName, what);

    /* Latch BEFORE anything else touches the slot: unload_resident() and the param reset
     * below both mutate state a reader would want, and a second fault during recovery
     * must not lose the first record. */
    const size_t slotIndex = static_cast<size_t>(&slot - sSlots);
    if (slotIndex < kMaxExtensions) {
        FaultRecord &rec = sFaults[slotIndex];
        const uint32_t previous = rec.valid ? rec.count : 0;
        rec.valid = true;
        strncpy(rec.name, slot.meta.displayName, sizeof(rec.name) - 1);
        rec.name[sizeof(rec.name) - 1] = '\0';
        rec.what = what;
        rec.uptimeMs = k_uptime_get();
        rec.cpuUs = cpuUs;
        rec.wallUs = wallUs;
        rec.count = previous + 1;
        rec.paramsReset = resetParams;
    }

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
     * the write would land on another slot's key. The shuffle-include flag (issue
     * #243) is deliberately NOT reset here: it is never handed to the extension, so
     * it cannot have caused the crash, and the user's opt-out must survive the
     * fault (fault exclusion from shuffle is isFaulted()'s job). */
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
 * Runs in exception context: keep it minimal.
 *
 * Coredump reboot policy (issue #80): by the time this handler runs, the
 * coredump (if enabled) has ALREADY been written to the coredump_partition —
 * z_fatal_error() calls coredump() before the handler. When no debugger is
 * attached (DHCSR C_DEBUGEN clear), reboot instead of halting so the device
 * recovers and coredump_manager can copy the dump to /NAND:/coredump on the
 * next boot. Under a debugger the halt is kept so GDB still sees the fault
 * live. sys_reboot() is safe here — on Nordic it's NVIC_SystemReset(), valid
 * in handler mode with IRQs locked. */
extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    ARG_UNUSED(esf);
    if (reason != K_ERR_KERNEL_PANIC &&
        k_current_get() == &extension_host::sSandboxThread) {
        LOG_ERR("fault (reason %u) in extension sandbox — aborting only the sandbox thread",
                reason);
        return;
    }
    log_panic();
#if defined(CONFIG_APP_COREDUMP_MANAGER)
    if ((CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) == 0) {
        LOG_ERR("Fatal error (reason %u) — rebooting to preserve coredump", reason);
        sys_reboot(SYS_REBOOT_COLD);
    }
#endif
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

/* Resolved required exports of a loaded extension (+ nullable optional ones). */
struct Exports {
    const struct rgbx_manifest *manifest;
    struct rgbx_inputs *inputs;
    uint8_t *framebuffer;
    void (*initFn)();
    void (*tickFn)();
    uint8_t *goodMoment; /* optional (issue #121); nullptr = absent */
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

    /* Optional exports: nullptr is a valid outcome (capability negotiation by symbol
     * presence — see the "Optional exports" section of rgbx_api.h), so this lookup is
     * deliberately excluded from the missing-required check below. */
    out.goodMoment = static_cast<uint8_t *>(
        const_cast<void *>(llext_find_sym(&ext->exp_tab, RGBX_SYM_GOOD_MOMENT)));

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

    /* An ABSENT optional export is fine; a PRESENT one is bounds-checked exactly like
     * the required data exports (untrusted-pointer rule — the kernel reads it). */
    if (exports.goodMoment != nullptr &&
        !in_ext_memory(ext, exports.goodMoment, sizeof(uint8_t))) {
        LOG_ERR("%s: rgbx_good_moment export outside extension memory", path);
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

/* The shuffle-include flag's load/save callbacks (issue #243): a plain 1-byte bool
 * under the slot's shuffleKey, loaded by the same settings_load_subtree("appcfg/ext")
 * pass as the param blob (the key lives inside that subtree). */
void ext_shuffle_do_load(void *target, const void *data, size_t len) {
    if (len != sizeof(bool)) {
        return;
    }
    auto *slot = static_cast<Slot *>(target);
    bool loaded;
    memcpy(&loaded, data, sizeof(loaded));
    slot->shuffleInclude = loaded;
}

void ext_shuffle_do_save(void *target) {
    auto *slot = static_cast<Slot *>(target);
    /* A bool copy can't tear — no sHostLock needed (unlike the param blob above). */
    bool current = slot->shuffleInclude;
    persistent_value_store::save_value(slot->shuffleKey, &current, sizeof(current));
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
    /* The registry fills the caller-owned record and links it by pointer (issue #114);
     * the slot outlives the registration. */
    int ret = persistent_value_registry_register(&slot.persistEntry, slot.settingsKey, &slot,
                                                 ext_params_do_load, ext_params_do_save);
    if (ret == 0) {
        slot.persistRegistered = true;
    } else {
        LOG_WRN("extension '%s': param persistence unavailable (%d) - values won't survive reboot",
                slot.meta.displayName, ret);
    }

    /* Second registration for the shuffle-include flag (issue #243). Same -EEXIST
     * semantics: a display-name collision leaves shufflePersistRegistered false so
     * this slot never dirties the key the other slot owns. */
    ret = persistent_value_registry_register(&slot.shufflePersistEntry, slot.shuffleKey, &slot,
                                             ext_shuffle_do_load, ext_shuffle_do_save);
    if (ret == 0) {
        slot.shufflePersistRegistered = true;
    } else {
        LOG_WRN("extension '%s': shuffle-include persistence unavailable (%d) - flag won't survive "
                "reboot",
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
        extension_param_persistence::build_shuffle_settings_key(
            slot.shuffleKey, sizeof(slot.shuffleKey), slot.meta.displayName);
    }
    /* Include in shuffle until the user (persisted value, loaded later in init(), or a
     * BLE/shell write) says otherwise. */
    slot.shuffleInclude = true;

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
/* @param deadline Shared wall-clock deadline owned by the calling tick(), so
 * this load's rgbx_init handshake and the tick that follows it draw on ONE
 * backstop between them rather than one each. */
bool runtime_load(size_t slotIndex, k_timepoint_t deadline) {
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
    sResident.goodMoment = exports.goodMoment;

    /* Wait for the extension's rgbx_init() to finish (same budget as a tick —
     * init runs sandboxed too, and a hang there must not stall the pattern
     * controller forever). This path is if anything MORE starvation-prone
     * than a steady-state tick (issue #276): activation happens during
     * animation switches and at boot, when the display and BT threads are
     * busiest, and rgbx_init also runs the extension's C++ static
     * constructors. Budgeting CPU rather than wall time is what keeps a
     * healthy extension from failing to activate under load. */
    uint32_t initCpuCyc = 0;
    uint32_t initWallCyc = 0;
    const TickVerdict initVerdict =
        wait_for_sandbox(deadline, /*postRequest=*/false, initCpuCyc, initWallCyc);
    if (initVerdict != TickVerdict::Completed) {
        LOG_ERR("%s: rgbx_init() %s (cpu %u us, wall %u us)", path, verdict_describe(initVerdict),
                k_cyc_to_us_near32(initCpuCyc), k_cyc_to_us_near32(initWallCyc));
        unload_resident();
        return false;
    }

    slot.tickWall.reset();
    slot.tickCpu.reset();
    slot.goodMoment = true;

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

bool isRetired(size_t slot) {
    return slot < sSlotCount && sSlots[slot].retired;
}

void retire(size_t slot) {
    if (slot >= sSlotCount) {
        return;
    }
    HostLockGuard lock;
    sSlots[slot].retired = true;
}

void unretire(size_t slot) {
    if (slot >= sSlotCount) {
        return;
    }
    HostLockGuard lock;
    sSlots[slot].retired = false;
}

int unlinkQuiesced(const char *path) {
    /* Holding sHostLock across the unlink serializes it against every host
     * FAT read (scan_slot's transient loads, runtime_load's lazy load) — see
     * the header. The unlink itself is short; the wait is bounded by one
     * in-flight load. */
    HostLockGuard lock;
    return fs_unlink(path);
}

int activeSlot() {
    HostLockGuard lock;
    return sActiveSlot;
}

const char *fileName(size_t slot) {
    if (slot >= sSlotCount) {
        return nullptr;
    }
    return extension_registry::name(sSlots[slot].fileIndex);
}

int findSlotByFileName(const char *name) {
    if (name == nullptr) {
        return -1;
    }
    for (size_t slot = 0; slot < sSlotCount; slot++) {
        const char *slotFile = extension_registry::name(sSlots[slot].fileIndex);
        /* Case-insensitive to match FatFs name semantics — see the header. */
        if (slotFile != nullptr && strcasecmp(slotFile, name) == 0) {
            return static_cast<int>(slot);
        }
    }
    return -1;
}

void purgePersistence(size_t slot) {
    if (slot >= sSlotCount || !IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG)) {
        return;
    }
    Slot &s = sSlots[slot];
    /* Ownership rule (see the Slot fields): only purge keys THIS slot
     * registered — a display-name-collision loser owns nothing, and purging
     * by key string alone would erase the surviving extension's records.
     *
     * The flags are read-and-cleared under sHostLock (they're written by
     * setParamValue()/writeParamString() paths on the BT RX thread under the
     * same lock), but the purge_value() submits happen AFTER releasing it:
     * the queued ext_params_do_save the purge serializes against takes
     * sHostLock itself on the same workqueue. Clearing the flags first is
     * what makes the window safe: no new mark_dirty can be issued for these
     * keys once they're false, and the async purge (see the header) then
     * unregisters + deletes without this thread ever waiting on the
     * persistence workqueue. */
    bool purgeParams;
    bool purgeShuffle;
    {
        HostLockGuard lock;
        purgeParams = s.persistRegistered;
        s.persistRegistered = false;
        purgeShuffle = s.shufflePersistRegistered;
        s.shufflePersistRegistered = false;
    }
    if (purgeParams) {
        persistent_value_store::purge_value(&s.persistPurge, &s.persistEntry, s.settingsKey);
    }
    if (purgeShuffle) {
        persistent_value_store::purge_value(&s.shufflePurge, &s.shufflePersistEntry, s.shuffleKey);
    }
}

bool atGoodSwitchPoint(size_t slot) {
    /* Anything not actively rendering (invalid slot, faulted, retired,
     * mid-lazy-load) reports true so shuffle mode can always switch away from
     * a fault banner and can never be pinned by an extension that never
     * finishes loading. */
    if (slot >= sSlotCount || sSlots[slot].faulted || sSlots[slot].retired) {
        return true;
    }
    return sSlots[slot].goodMoment;
}

bool shuffleIncluded(size_t slot) {
    return slot < sSlotCount && sSlots[slot].shuffleInclude;
}

void setShuffleInclude(size_t slot, bool include) {
    if (slot >= sSlotCount) {
        return;
    }
    sSlots[slot].shuffleInclude = include;
    /* Only persist if this slot owns its key (see shufflePersistRegistered) — same
     * duplicate-display-name rule as setParamValue() above. */
    if (IS_ENABLED(CONFIG_APP_PERSIST_BT_CONFIG) && sSlots[slot].shufflePersistRegistered) {
        persistent_value_registry_mark_dirty(sSlots[slot].shuffleKey);
        persistent_value_store::request_save();
    }
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

    /* A retired slot's file is gone (runtime FILE_MGMT delete) — the lazy
     * load below could only fail its FAT read, so reject up front. Unlike a
     * fault there is no reset path: the file does not come back. */
    if (s.retired) {
        LOG_WRN("extension '%s' was deleted — activation rejected (restart to rescan)",
                s.meta.displayName);
        return false;
    }

    /* Cheap host-side work only (this commonly runs on the BLE RX thread):
     * tear down whatever is resident and defer the FAT load + sandbox
     * bring-up to the pattern-controller thread's first tick(). */
    unload_resident();
    sActiveSlot = static_cast<int>(slot);
    sPendingLoadSlot = static_cast<int>(slot);
    /* Arm a color-mode state reset (fresh RandomOnActivate roll, restarted
     * sweep phase) on every resolver — cheap, and covers slot switches where
     * the same param index is a COLOR param in both slots. */
    for (auto &resolver : sParamColorResolvers) {
        resolver.mode.notifyActivated();
    }
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
    /* Deliberately does NOT clear `retired`: a fault is recoverable (the code
     * is still on disk, `ext select` retries it), but a retired slot's file
     * was deleted — nothing to retry until the next boot rescan. */
    sSlots[slot].faulted = false;
}

bool tick(size_t slot, uint32_t dtMs, AnimationRenderer &renderer) {
    /* Held across the whole handshake (and the one-time lazy load): an
     * Is Active write on the BT RX thread must not abort/recreate the
     * sandbox thread while this tick is between the request and done
     * semaphores.
     *
     * Worst-case hold time is therefore the handshake's worst case, and CPU
     * budgeting (issue #276) changed that bound: it used to be a flat
     * CONFIG_APP_EXT_TICK_DEADLINE_MS, because a starved tick was simply
     * declared dead at 50 ms. Now a starved-but-healthy tick is correctly
     * waited out, so the hold lasts as long as the sandbox actually takes to
     * get scheduled, and only a genuinely blocked extension runs to the
     * deadline below. Concurrent BLE param writes and `ext select` block for
     * that long, on ANY slot, since they share this mutex. Measured on proto0
     * under app-connected load: worst handshake wall time 8.3 ms against a
     * 500 ms backstop — reached only in the fault case, once, immediately
     * before the extension is unloaded. Keep that ratio in mind before raising
     * the backstop, and see fw/docs/threading.md for the full trade-off. */
    HostLockGuard lock;
    if (!isLoaded(slot) || sSlots[slot].faulted || sActiveSlot != static_cast<int>(slot)) {
        return false;
    }
    Slot &s = sSlots[slot];

    /* ONE deadline for everything this call does under the lock. The lazy load
     * below runs its own full rgbx_init handshake before the tick handshake
     * that follows it; giving each its own backstop would let a single tick()
     * hold sHostLock for twice the advertised ceiling. */
    const k_timepoint_t deadline =
        sys_timepoint_calc(K_MSEC(CONFIG_APP_EXT_TICK_WALL_BACKSTOP_MS));

    if (sPendingLoadSlot == static_cast<int>(slot)) {
        sPendingLoadSlot = -1;
        if (!runtime_load(slot, deadline)) {
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
    /* Resolve color-mode metadata (issue #259): COLOR params reach the
     * extension as the effective 0x00RRGGBB while paramValues stays the raw
     * mode-carrying value (authoritative for BLE reads and persistence). */
    for (size_t p = 0; p < s.meta.paramCount; p++) {
        if (s.meta.params[p].type == RGBX_PARAM_COLOR) {
            in->params[p] = sParamColorResolvers[p].mode.get();
        }
    }
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

    uint32_t tickCpuCyc = 0;
    uint32_t tickWallCyc = 0;
    /* wait_for_sandbox() posts the request itself, so the measurement origin is
     * taken before it rather than after (issue #276 review). */
    const TickVerdict verdict =
        wait_for_sandbox(deadline, /*postRequest=*/true, tickCpuCyc, tickWallCyc);
    if (verdict_is_fault(verdict)) {
        /* Budget blown — the extension is spinning, blocked, or it MPU-faulted
         * (Zephyr already aborted the thread in that case; aborting again is
         * harmless). EVERY tick-time fault clears the slot's params: any of the
         * three can be driven by a poisoned persisted value, including the
         * blocked case — an extension that burns no CPU is often blocked
         * precisely because a parameter sent it down a waiting path. Sparing
         * any of them leaves the slot unable to self-recover across `ext
         * select` or a reboot. See the NOTE in extension_tick_budget.h. */
        sandbox_fault(s, verdict_describe(verdict), /*resetParams=*/true,
                      k_cyc_to_us_near32(tickCpuCyc), k_cyc_to_us_near32(tickWallCyc));
        return false;
    }
    s.tickWall.record(tickWallCyc);
    s.tickCpu.record(tickCpuCyc);

    /* The sandbox is quiescent between the done-semaphore and the next request, so this
     * is the one safe point to read the extension's per-tick outputs. Absent symbol =
     * always a good moment (same default as built-in animations). */
    s.goodMoment = (sResident.goodMoment == nullptr) || (*sResident.goodMoment != 0);

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
        shell_print(sh, "[%zu] id=0x%02x '%s' file=%s params=%zu%s%s%s", i,
                    (unsigned)(kAnimationIdBase + i), sSlots[i].meta.displayName,
                    extension_registry::name(sSlots[i].fileIndex), sSlots[i].meta.paramCount,
                    sActiveSlot == (int)i ? " [active]" : "",
                    sSlots[i].faulted ? " [FAULTED]" : "",
                    sSlots[i].retired ? " [RETIRED - file deleted, restart to rescan]" : "");
    }
    return 0;
}

int cmd_ext_stats(const struct shell *sh, size_t, char **) {
    for (size_t i = 0; i < sSlotCount; i++) {
        const Slot &s = sSlots[i];
        if (s.tickWall.count == 0) {
            shell_print(sh, "[%zu] '%s': no ticks recorded", i, s.meta.displayName);
            continue;
        }
        /* CPU is what the budget is enforced against; wall is what the render
         * loop actually waited. A large gap between them is preemption, not a
         * slow extension (issue #276) — printing only one number is what made
         * that misdiagnosable. */
        shell_print(sh, "[%zu] '%s': %u ticks", i, s.meta.displayName, s.tickWall.count);
        shell_print(sh, "      cpu  min/avg/max = %u/%u/%u us  (budget %u us)",
                    k_cyc_to_us_near32(s.tickCpu.minCyc), k_cyc_to_us_near32(s.tickCpu.avgCyc()),
                    k_cyc_to_us_near32(s.tickCpu.maxCyc),
                    (unsigned)CONFIG_APP_EXT_TICK_CPU_BUDGET_MS * 1000U);
        shell_print(sh, "      wall min/avg/max = %u/%u/%u us  (backstop %u us)",
                    k_cyc_to_us_near32(s.tickWall.minCyc), k_cyc_to_us_near32(s.tickWall.avgCyc()),
                    k_cyc_to_us_near32(s.tickWall.maxCyc),
                    (unsigned)CONFIG_APP_EXT_TICK_WALL_BACKSTOP_MS * 1000U);
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

int cmd_ext_shuffle(const struct shell *sh, size_t argc, char **argv) {
    if (argc != 2 && argc != 3) {
        shell_error(sh, "Usage: ext shuffle <slot> [0|1]");
        return -EINVAL;
    }
    size_t slot = strtoul(argv[1], nullptr, 10);
    if (!isLoaded(slot)) {
        shell_error(sh, "no extension in slot %zu", slot);
        return -ENOENT;
    }
    if (argc == 3) {
        setShuffleInclude(slot, strtoul(argv[2], nullptr, 0) != 0);
        /* No BLE push: the characteristic is no longer notifiable (Android
         * registration-budget fix) — a connected app picks the change up on
         * its next read of the characteristic. */
    }
    shell_print(sh, "%s.shuffle_include = %u", sSlots[slot].meta.displayName,
                shuffleIncluded(slot) ? 1 : 0);
    return 0;
}

/* Reports every latched fault. Deliberately prints even for slots that are no longer
 * faulted: the case this exists for is a TRANSIENT fault that cleared on the next
 * `ext select`, which `ext list` shows as perfectly healthy. */
int cmd_ext_faults(const struct shell *sh, size_t, char **) {
    size_t shown = 0;
    for (size_t i = 0; i < kMaxExtensions; i++) {
        const FaultRecord &rec = sFaults[i];
        if (!rec.valid) {
            continue;
        }
        shown++;

        const int64_t ageMs = k_uptime_get() - rec.uptimeMs;
        shell_print(sh, "[%zu] '%s': %s", i, rec.name, rec.what);
        shell_print(sh, "      at %lld ms uptime (%lld ms ago), %u time(s) since clear",
                    rec.uptimeMs, ageMs, rec.count);

        if (rec.cpuUs != 0 || rec.wallUs != 0) {
            /* The pair is the diagnosis, not decoration. cpu near zero against a large
             * wall means the sandbox was BLOCKED or starved by something else on the
             * system — not a runaway extension — which is the difference between
             * chasing the extension and chasing the scheduler (issue #276). */
            shell_print(sh, "      cpu %u us / wall %u us  (budget %u us / backstop %u us)",
                        rec.cpuUs, rec.wallUs,
                        (unsigned)CONFIG_APP_EXT_TICK_CPU_BUDGET_MS * 1000U,
                        (unsigned)CONFIG_APP_EXT_TICK_WALL_BACKSTOP_MS * 1000U);
            if (rec.wallUs > 4 * (rec.cpuUs + 1)) {
                shell_print(sh, "      ^ wall >> cpu: blocked or starved, NOT a runaway "
                                "extension");
            }
        } else {
            shell_print(sh, "      load/init-time failure — no tick was measured");
        }
        shell_print(sh, "      params reset to manifest defaults: %s",
                    rec.paramsReset ? "yes" : "no");
        shell_print(sh, "      currently: %s", sSlots[i].faulted ? "FAULTED" : "recovered");
    }

    if (shown == 0) {
        shell_print(sh, "no extension faults recorded since boot (or since the last clear)");
    } else {
        shell_print(sh, "(latched until `ext faults clear`)");
    }
    return 0;
}

/* Clearing only drops the RECORDS, never a slot's faulted state — a faulted slot still
 * needs `ext select <slot>` to retry. Conflating the two would let an operator think they
 * had recovered an extension by clearing a log. */
int cmd_ext_faults_clear(const struct shell *sh, size_t, char **) {
    for (size_t i = 0; i < kMaxExtensions; i++) {
        sFaults[i] = FaultRecord{};
    }
    shell_print(sh, "fault records cleared (slot fault state unchanged — "
                    "use `ext select <slot>` to retry a faulted extension)");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_ext_faults,
                               SHELL_CMD(clear, NULL, "Clear the latched fault records",
                                         cmd_ext_faults_clear),
                               SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_ext, SHELL_CMD(list, NULL, "List loaded animation extensions", cmd_ext_list),
    SHELL_CMD(faults, &sub_ext_faults, "Latched extension fault details (see also: clear)",
              cmd_ext_faults),
    SHELL_CMD(stats, NULL, "Per-extension tick timing: cpu vs wall (min/avg/max us)",
              cmd_ext_stats),
    SHELL_CMD_ARG(param, NULL, "Get/set a param: ext param <slot> <index> [<value>]",
                  cmd_ext_param, 3, 1),
    SHELL_CMD_ARG(select, NULL,
                  "Activate extension animation (clears a fault): ext select <slot>",
                  cmd_ext_select, 2, 0),
    SHELL_CMD_ARG(shuffle, NULL,
                  "Get/set include-in-shuffle (issue #243): ext shuffle <slot> [0|1]",
                  cmd_ext_shuffle, 2, 1),
    SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(ext, &sub_ext, "Animation extension host", NULL);

}  // namespace
}  // namespace extension_host
