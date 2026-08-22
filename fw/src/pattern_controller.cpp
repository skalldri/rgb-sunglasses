#include <animations/active_animation_binding.h>
#include <animations/animation_registry.h>
#include <animations/animation_renderer.h>
#include <animations/bt_animations.h>
#include <animations/null_animation.h>
#include <bluetooth/boot_gate.h>
#include <configuration_provider.h>
#if defined(CONFIG_STATUS_LED)
#include <status_led/status_led.h>
#endif
#include <core_config.h>
#include <led_controller.h>
#include <pattern_controller.h>
#include <render_pacing.h>

#if defined(CONFIG_FAT_FILESYSTEM_ELM)
#include <storage/glim_registry.h>
#endif
#if defined(CONFIG_APP_EXTENSION_HOST)
#include <extensions/extension_host.h>
#include <extensions/extension_limits.h>
#endif
#if defined(CONFIG_APP_SHUFFLE)
#include <animations/shuffle_controller.h>
#include <bluetooth/shuffle_service.h>
#include <zephyr/random/random.h>
#endif
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include <cstring>

LOG_MODULE_REGISTER(pattern_controller, LOG_LEVEL_INF);

static ConfigurationProvider *sConfigProvider = nullptr;

// Render-tick epoch (see pattern_controller.h): bumped once at the top of each
// render tick; consumers compare for equality only.
static atomic_t sTickEpoch = ATOMIC_INIT(0);

uint32_t pattern_controller_tick_epoch(void) {
    return (uint32_t)atomic_get(&sTickEpoch);
}

void pattern_controller_set_config_provider(ConfigurationProvider *provider) {
    sConfigProvider = provider;
}

static ConfigurationProvider &getPatternConfig() {
    if (!sConfigProvider) {
        sConfigProvider = &CoreConfig::getInstance();
    }
    return *sConfigProvider;
}

// PatternControllerBtObserver (the BtStateObserver driving the indicator overlay and
// status LED) lives in src/pattern_controller_bt_observer.cpp so it can be unit-tested
// without linking this TU's render thread / LED controller / FAT / extension host.

void pattern_controller_thread_func(void *a, void *b, void *c);

// Kernel-only thread: K_KERNEL_* skips the 1KB CONFIG_USERSPACE privileged stack; this
// stack can never host a K_USER thread. (Extension code runs on the extension host's own
// K_USER sandbox thread, not this one.)
K_KERNEL_THREAD_DEFINE(pattern_controller_thread, CONFIG_APP_PATTERN_CONTROLLER_THREAD_STACK_SIZE,
                       pattern_controller_thread_func, NULL, NULL, NULL,
                       CONFIG_APP_PATTERN_CONTROLLER_THREAD_PRIORITY, 0, 0);

// This thread does FAT/QSPI I/O (GLIM assets, .llext loads) and settings persistence, so
// it must stay preemptible — a long flash operation from a cooperative thread starves the
// whole system (see fw/CLAUDE.md's coding rules and fw/docs/threading.md).
BUILD_ASSERT(CONFIG_APP_PATTERN_CONTROLLER_THREAD_PRIORITY >= 0,
             "pattern_controller_thread does flash I/O and must be preemptible");
BUILD_ASSERT(CONFIG_APP_PATTERN_CONTROLLER_THREAD_PRIORITY < CONFIG_NUM_PREEMPT_PRIORITIES,
             "CONFIG_APP_PATTERN_CONTROLLER_THREAD_PRIORITY is outside the preemptible range");

// Intentionally unsynchronized: written from the BT thread (via
// PatternControllerBtObserver), the shell thread (`anim indicator ...`) and this file's
// own thread, and read every render tick. Each access is a single aligned enum store or
// load, and every interleaving lands on a valid Indicator value — the worst outcome is
// one frame rendering the previous overlay. The observer's read-then-clear
// (pattern_controller_bt_observer.cpp) is a check-then-act on this variable for the same
// reason: converging on None/BtAdvertising either way is benign. Don't add a lock here
// without checking the render path's timing budget.
Indicator currentIndicator = Indicator::None;
Animation currentAnimation = Animation::None;

float sBrightnessForFrame = 0.1f;

namespace {

/* The active animation is deliberately NOT persisted (issue #311).
 *
 * It used to be, under "core/last_active_animation" plus a companion
 * "core/last_active_extension" name key. Every user-initiated switch marked
 * both dirty, so each switch cost one settings write AND one delete of the
 * sibling key. Measured on proto0 running fw-v3.2.0, per switch:
 *
 *   built-in -> built-in     ~850-900 ms
 *   built-in -> extension    ~920 ms
 *   extension -> built-in   ~1400 ms
 *   over BLE, to an extension: a 1504 ms save, inside which the LED display
 *                              thread stalled for 1035 ms (one visible freeze
 *                              about a second after the switch)
 *
 * The cost is not the flash write itself. CONFIG_SETTINGS_NVS_NAME_CACHE is
 * what makes a settings lookup cheap; without it, settings_nvs_save() walks
 * every name id doing a full NVS scan per id whenever the name is not found,
 * which is exactly what a delete-of-an-absent-key and a first-write-of-a-new-
 * key both do. That cache is now enabled (in fw/prj.conf, so it applies to every
 * board built from it — the overflow budget has to hold for all of them), but the
 * write is removed as well: an animation switch is a per-interaction event, and
 * the NAND has finite erase/write cycles to spend on things the user actually
 * asked to keep.
 *
 * Two consequences, both accepted deliberately:
 *
 *  1. The device always boots to the default animation below. Nothing restores
 *     the pre-power-cycle selection.
 *  2. "All animations off" no longer survives a power cycle either. Animation::None
 *     IS a registered animation (null_animation_factory, animation_registry_defaults.cpp),
 *     and turning everything off in the app routes to it via
 *     PatternControllerActivator::deactivateAnimation — so the old restore path
 *     booted such a device dark. It now comes up lit on the default instead.
 *     That is a real loss, not just a convenience one: the panel lights up
 *     without being asked and draws current. Persisting even a single "booted
 *     off" bool was considered and rejected — it is still a flash write on a user
 *     interaction, which is the whole thing this change removes.
 *
 * Do not reintroduce a write on this path. If boot restore (or the off state) is
 * ever wanted again, it needs a trigger that is not "every switch" — an
 * idle/disconnect checkpoint, or storage that is not NVS. */

#if defined(CONFIG_APP_SHUFFLE)

// Production adapters for the ShuffleController seams (issue #121): the pool is the
// animation registry (which already contains extension slots), the config is the
// dedicated Shuffle service's characteristics (issue #243, previously Core Config).
class RegistryShufflePool : public ShuffleAnimationPool {
   public:
    size_t count() override { return animation_registry_count(); }
    Animation idAt(size_t index) override { return animation_registry_id_at(index); }
    bool isEligible(Animation id) override {
        if (id == Animation::None) {
            return false;  // "all LEDs off" is not a show entry
        }
#if defined(CONFIG_APP_EXTENSION_HOST)
        const uint32_t v = static_cast<uint32_t>(id);
        if (v >= extension_host::kAnimationIdBase) {
            const size_t slot = v - extension_host::kAnimationIdBase;
            if (extension_host::isFaulted(slot)) {
                return false;  // never shuffle INTO a faulted extension (escape is separate)
            }
            if (extension_host::isRetired(slot)) {
                return false;  // file deleted at runtime — activation would only fail
            }
            // Extensions consult the host directly (same precedent as isFaulted) rather
            // than a registry getter — avoids another post-proxy registration whose
            // ignored failure would be the PR #89 silent-miss class.
            return extension_host::shuffleIncluded(slot);
        }
#endif
        // Built-ins: the per-animation "Include in Shuffle" characteristic, pulled
        // through the BT-free registry binding (issue #243). Default true.
        return animation_registry_shuffle_included(id);
    }
};

class ShuffleServiceConfigSource : public ShuffleConfigSource {
   public:
    bool enabled() override { return shuffle_service_get_enabled(); }
    uint32_t minDurationS() override { return shuffle_service_get_min_duration_s(); }
    uint32_t maxDurationS() override { return shuffle_service_get_max_duration_s(); }
};

RegistryShufflePool sShufflePool;
ShuffleServiceConfigSource sShuffleConfigSource;
// sys_rand32_get is entropy-seeded on proto0 (same source matrix_code_animation uses).
ShuffleController sShuffleController(
    sShufflePool, sShuffleConfigSource, sys_rand32_get,
    static_cast<uint64_t>(CONFIG_APP_SHUFFLE_GRACE_S) * 1000u,
    static_cast<uint64_t>(CONFIG_APP_SHUFFLE_MAX_GRACE_S) * 1000u);

#endif  // CONFIG_APP_SHUFFLE

}  // namespace

BaseAnimation *getIndicator(Indicator indicator) {
    switch (indicator) {
        case Indicator::BtConnecting:
            return BtConnectingAnimation::getInstance();

        case Indicator::BtAdvertising:
            return BtAdvertisingAnimation::getInstance();

        case Indicator::BtPairing:
            return BtPairingAnimation::getInstance();

        case Indicator::ExtensionsLoading:
            return BtExtensionsLoadingAnimation::getInstance();

        case Indicator::None:
            // Explicit fallthrough to get to the NULL animation
            break;
    }

    return NULL;
}

BaseAnimation *getAnimation(Animation animation) {
    return animation_registry_get(animation);
}

BaseAnimation *getBestRenderAnimation() {
    // Indicators have priority over other animation types
    if (currentIndicator != Indicator::None) {
        return getIndicator(currentIndicator);
    }

    return getAnimation(currentAnimation);
}

void pattern_controller_thread_func(void *a, void *b, void *c) {
    int ret;

    // LOG_INF("Pattern control thread start!");

    // Initialize all animations
    BtAdvertisingAnimation::getInstance()->init();
    BtConnectingAnimation::getInstance()->init();
    BtPairingAnimation::getInstance()->init();
    BtExtensionsLoadingAnimation::getInstance()->init();

    // Boot notification (issue #208): show this on the BLE status LED while extension
    // discovery/registration runs below, so it's visible for however long
    // boot_gate_wait_ready() ends up blocking bt_thread's first advertising start.
    // Distinct from every BtStateObserver-driven indicator (all blue) and from the power
    // LED (never blue or violet) by using violet here instead.
#if !defined(CONFIG_STATUS_LED)
    pattern_controller_request_indicator(Indicator::ExtensionsLoading);
#else
    status_led_set(1, StatusIndication::FastBreathing, StatusColor::Violet);
#endif

#if defined(CONFIG_ANIMATION_GLIM_PLAYER)
    // Must run before animation_registry_register_defaults(), which seeds the Glim Player's
    // BLE selection characteristic from whatever this discovers. Gated on the animation itself
    // (not just CONFIG_FAT_FILESYSTEM_ELM) - glim_registry is only used by Glim Player, so a
    // build that enables FAT for some other reason shouldn't pay for this filesystem scan.
    glim_registry::init();
#endif

    ret = animation_registry_register_defaults();
    if (ret) {
        LOG_ERR("Failed to register default animations: %d", ret);
    } else {
#if defined(CONFIG_APP_EXTENSION_HOST)
        // Discover, load, and register sandboxed animation extensions (issue #85).
        // Must run here (kernel-mode thread context): it scans the FAT filesystem
        // (fs_* has no syscall coverage) after the prio-90 SYS_INIT mount, and it
        // registers into the animation registry, so it goes between
        // register_defaults() and init_registered().
        extension_host::init();
#endif
        animation_registry_init_registered();
    }

    // Boot-time work that bt_thread's first advertising start is gated on (issue #208) is
    // done - signal it unconditionally, including the ret-failure path above, so a
    // registration failure here can't also permanently block BLE advertising.
    boot_gate_notify_ready();

    // Always start from the default. The previously-active animation is no longer
    // persisted — see the comment at the top of the anonymous namespace for the
    // measured per-switch cost that bought this, and for what any future boot-restore
    // would have to avoid. Records written by earlier firmware under
    // "appcfg/core/last_active_animation" / "appcfg/core/last_active_extension" are
    // functionally inert, but NOT silent: the "appcfg" subtree handler still matches
    // them, persistent_value_registry_dispatch_load() finds no registered entry and
    // returns -ENOENT, and settings_call_set_handler() logs
    //     <err> settings: set-value failure. key: appcfg/core/last_active_... error(-2)
    // once per key at every boot before swallowing it. That is expected on any device
    // upgraded from a build that wrote them, and it is the same line already seen for
    // orphaned "appcfg/ext/<name>" blobs. Do not chase it as a fault.
    // They are deliberately not deleted — a one-shot migration delete would be exactly
    // the unnecessary NVS write this change removes.
#if defined(CONFIG_APP_WASM3_MVP)
    pattern_controller_change_to_animation(Animation::WasmMvp);
#else
    pattern_controller_change_to_animation(Animation::ZigZag);
#endif

    // Overrun logging is rate-limited (see the overrun branch at the bottom of the loop).
    int64_t lastRenderOverrunLogMs = 0;
    uint32_t renderOverrunsSinceLog = 0;
    int64_t lastWaitTimeoutLogMs = 0;
    uint32_t waitTimeoutsSinceLog = 0;
    int64_t prevIterStartTicks = k_uptime_ticks();
#if defined(CONFIG_APP_SHUFFLE)
    // Fractional-ms carry for the shuffle dwell clock (see the onFrame call).
    float shuffleDtRemainderMs = 0.0f;
#endif

    while (true) {
        int64_t startTicks = k_uptime_ticks();
        // New render tick: everything below (animation update() drains, color
        // sources, extension inputs) belongs to this epoch — see
        // pattern_controller_tick_epoch().
        atomic_inc(&sTickEpoch);

        // Producer-gated pacing (issue #379): this thread no longer runs its own
        // free-running sleep clock — it renders once per N consumed display
        // frames (see the wait at the bottom of the loop), so its cadence is
        // slaved to the display thread's clock and every display push samples
        // exactly one fresh frame. core/render_thread_rate_ms therefore acts as
        // a DIVIDER — the arithmetic (ceiling semantics, unusable-display
        // fallback) lives in the pure seam render_pacing.h so it is
        // native_sim-testable (fw/tests/render_pacing/), since this loop itself
        // is not compiled into any test binary (PR #381 review). At the
        // defaults N == 1 and dt is 33.3 ms, exactly as before.
        const float displayIntervalMs = getPatternConfig().getDisplayRateMs();
        const float renderRateMs = getPatternConfig().getRenderRateMs();
        const uint32_t framesPerRender =
            render_pacing::framesPerRender(renderRateMs, displayIntervalMs);
        // The NOMINAL interval: the overrun threshold at the bottom of the loop
        // and the wait-timeout base. Deliberately never substituted with wall
        // time — doing so would silently raise the overrun threshold to ~2x
        // N x display for the whole degraded stretch, disabling the overrun
        // telemetry exactly when someone is reading the log to find out why the
        // system is degraded (PR #381 review).
        const float kTargetRenderIntervalMs =
            render_pacing::renderIntervalMs(renderRateMs, displayIntervalMs);
        // The dt handed to the animations and shuffle: the wall time actually
        // elapsed since the previous iteration, every iteration — not the
        // nominal interval. The handshake makes this loop's real cadence a
        // function of the display thread's ACTUAL period, and a chronic
        // sub-timeout display slowdown (the display missing its budget under
        // BT/flash load — exactly what #267/#312 measured) never trips the
        // wait timeout below, so a nominal dt would run animation time slow
        // for as long as the slowdown lasts with nothing logged (PR #381
        // review round 6). Measured in kernel ticks, not k_uptime_get()'s
        // whole-ms truncation: at the 33.3 ms default a ms-quantised delta
        // alternates 33/34 — a ±2% per-frame velocity wobble in every
        // dt-integrating animation, the same stepping-irregularity class this
        // PR removes, re-injected at small amplitude (PR #381 review round 6;
        // startTicks is already sampled above, so this costs nothing). Clamped
        // to 4x nominal: the wait's shared deadline bounds a healthy iteration
        // well inside that, and a multi-second display stall (#312 measured
        // ~1.5 s) should not teleport animation clocks when it clears. A
        // same-tick iteration (elapsed 0 — unreachable in practice at 32768 Hz
        // resolution) keeps the nominal fallback, since dt 0 would stall
        // animation and shuffle-dwell clocks; a genuinely tiny elapsed (an
        // immediate take off a coalesced credit) is passed through truthfully.
        float animationDtMs = kTargetRenderIntervalMs;
        const int64_t elapsedTicks = startTicks - prevIterStartTicks;
        if (elapsedTicks > 0) {
            animationDtMs =
                1000.0f * (float)elapsedTicks / (float)CONFIG_SYS_CLOCK_TICKS_PER_SEC;
            const float maxDtMs = 4.0f * kTargetRenderIntervalMs;
            if (animationDtMs > maxDtMs) {
                animationDtMs = maxDtMs;
            }
        }
        prevIterStartTicks = startTicks;

        size_t bufferId = 0;
        ret = claimBufferForRender(bufferId);
        if (ret) {
            LOG_ERR("Failed to acquire render buffer!");
        } else {
            BaseAnimation *anim = getBestRenderAnimation();

            // Latch current brightness value from the configuration provider
            sBrightnessForFrame = getPatternConfig().getBrightnessFactor();

            class PatternControllerRenderer : public AnimationRenderer {
                const LedConfig *config_;
                size_t bufferId_;

               public:
                PatternControllerRenderer(const LedConfig *c, size_t b)
                    : config_(c), bufferId_(b) {}
                size_t displayWidth() const override { return config_->displayWidth; }
                size_t displayHeight() const override { return config_->displayHeight; }
                void setPixel(size_t x, size_t y, uint8_t r, uint8_t g, uint8_t b) override {
                    pattern_controller_set_pixel_in_framebuffer(config_, x, y, bufferId_, r, g, b);
                }
            };
            PatternControllerRenderer renderer(get_current_led_config(), bufferId);

            if (anim) {
                anim->tick(renderer, animationDtMs);
            } else {
                // No animation: default to all LEDs off to save power
                BaseAnimation *nullAnimation = animation_registry_get(Animation::None);
                if (nullAnimation) {
                    nullAnimation->tick(renderer, animationDtMs);
                }
            }

            ret = releaseBufferFromRender(bufferId);
            if (ret) {
                LOG_ERR("Failed to release render buffer!");
            }

#if defined(CONFIG_APP_SHUFFLE)
            // Shuffle step (issue #121): runs on this thread only — the one context
            // where change_to_animation + a lazy extension load are already done from
            // (see the boot restore below the loop head). Skipped while an indicator
            // overlay is active, since the indicator is what's actually ticking then and
            // the selected animation's good-moment flag would be stale.
            if (currentIndicator == Indicator::None) {
                BaseAnimation *shuffleCur = anim;  // == getAnimation(currentAnimation) here
                // onFrame takes whole ms, but animationDtMs is fractional
                // (~33.3): a bare truncating cast would drop ~0.3 ms EVERY
                // frame, running the shuffle dwell clock ~1% slow against the
                // animation's float clock (~0.55 s behind over a 60 s dwell;
                // ~3.9% at a 16.65 ms pair) — and with wall-clock dt the lost
                // fraction varies per frame rather than being a fixed offset
                // (PR #381 review round 9; the truncation itself predates this
                // PR). Carrying the fractional remainder across frames keeps
                // the two clocks agreeing in the long run, so a requested
                // "time to my next boundary" stays honest against the dwell
                // deadline it is compared with.
                const float shuffleDtTotalMs = animationDtMs + shuffleDtRemainderMs;
                const uint32_t shuffleDtMs = static_cast<uint32_t>(shuffleDtTotalMs);
                shuffleDtRemainderMs = shuffleDtTotalMs - static_cast<float>(shuffleDtMs);
                const ShuffleController::Decision d = sShuffleController.onFrame(
                    currentAnimation, shuffleDtMs,
                    shuffleCur ? shuffleCur->isAtGoodSwitchPoint() : true,
                    shuffleCur ? shuffleCur->goodSwitchPointGraceMs() : 0u);
                if (d.switchNow) {
                    // Shuffle hops never touched settings flash even when manual switches
                    // did; now nothing on this path does.
                    pattern_controller_change_to_animation(d.next);
                }
            }
#endif
        }

        int64_t endTicks = k_uptime_ticks();
        int64_t updateTicks = endTicks - startTicks;

        float updateTimeS = ((float)updateTicks) / ((float)CONFIG_SYS_CLOCK_TICKS_PER_SEC);
        float updateTimeMs = updateTimeS * 1000.0f;

        if (updateTimeMs > kTargetRenderIntervalMs) {
            renderOverrunsSinceLog++;
            // Rate-limited: at the render-rate default this fired once per render tick
            // under sustained load, which is the per-tick log spam PR #110 banned.
            const int64_t nowMs = k_uptime_get();
            if (nowMs - lastRenderOverrunLogMs >= 5000) {
                LOG_WRN("Render overran the tick interval %u time(s) in the last %lld ms — "
                        "cannot keep render rate",
                        renderOverrunsSinceLog, nowMs - lastRenderOverrunLogMs);
                lastRenderOverrunLogMs = nowMs;
                renderOverrunsSinceLog = 0;
            }
            // Yield unconditionally even after blowing the budget (PR #381
            // review): during a SUSTAINED overrun the display's coalesced
            // credit is already pending when the wait below runs, and
            // k_sem_take on an available semaphore returns without
            // rescheduling — so without this sleep the loop never yields and
            // everything below priority 4 starves (the prio-14 persist and
            // MCUboot-updater workqueues: OTA flash writes and debounced
            // settings saves would stall for as long as the overrun lasts).
            // Same latent trap as issue #267's in led_controller.cpp.
            k_msleep(1);
            // At N == 1 the immediate take after an overrun is the correct
            // catch-up (the display is about to want a fresh frame), and
            // consecutive immediate takes cannot outpace the display's
            // max-count-1 gives. Known N > 1 caveat (PR #381 review): gives
            // that land DURING an overlong render coalesce into one credit, so
            // a render spanning several display periods still waits N-1 more
            // periods afterwards — the effective interval can stretch past
            // N*display by the render's own overshoot. Accepted: a count-N
            // semaphore would let the render burst to catch up after a stall,
            // which is worse than briefly pacing slower than a non-default
            // divider asked for.
        }

        // Pace off the display clock (issue #379): wait until the display thread
        // has consumed N frames, then render the next one. Bounded, never
        // K_FOREVER — the display thread exits at boot when the LED strip devices
        // are not ready, and this thread must keep ticking (shuffle, indicators,
        // extension activation) rather than wedge; on timeout the loop breaks and
        // proceeds, and the next iteration's wall-clock dt (above) absorbs the
        // extra elapsed time. All N takes share ONE deadline of ~2x the
        // CONFIGURED render interval (2 x N x display + 5 ms), each take getting
        // only the remaining budget — a full timeout per take would let N
        // just-under-timeout takes stack to ~2 x N^2 x display, silently pausing
        // shuffle/indicator/extension servicing far past the documented bound
        // (PR #381 review round 6; threading.md's "~2 x N display periods"
        // invariant is this deadline). A per-display-period deadline with the
        // break would instead render dividers' frames N times too fast, burning
        // render/sandbox CPU on frames nobody pushes (PR #381 review). A timeout
        // can also fire during a genuine multi-hundred-ms display stall (#312
        // measured ~1.5 s of cooperative-band starvation) — that is benign: dt
        // stays truthful, the WARN is rate-limited, and the max-count-1
        // semaphore re-syncs the pacing on the display's next give.
        if (displayIntervalMs <= 0.0f) {
            // Display interval written to 0 (remotely writable, unclamped): the
            // display thread's own trailing k_msleep(0 - work) makes it a
            // spinner that gives a credit every iteration, so pacing off the
            // semaphore would make THIS thread a second unpaced spinner and
            // starve everything below priority 4 (PR #381 review). Self-pace
            // exactly as the pre-handshake code did — dt above already fell
            // back to getRenderRateMs() on this path.
            float sleepMs = kTargetRenderIntervalMs - updateTimeMs;
            if (sleepMs < 1.0f) {
                sleepMs = 1.0f;
            }
            k_msleep((int32_t)sleepMs);
            continue;
        }
        const int32_t waitTimeoutMs =
            (int32_t)(2.0f * (float)framesPerRender * displayIntervalMs) + 5;
        const int64_t waitDeadlineMs = k_uptime_get() + waitTimeoutMs;
        for (uint32_t consumed = 0; consumed < framesPerRender; consumed++) {
            const int64_t remainingMs = waitDeadlineMs - k_uptime_get();
            if (remainingMs <= 0 ||
                led_controller_wait_frame_consumed(K_MSEC((int32_t)remainingMs)) != 0) {
                waitTimeoutsSinceLog++;
                const int64_t nowMs = k_uptime_get();
                if (nowMs - lastWaitTimeoutLogMs >= 5000) {
                    LOG_WRN("Display consumed %u of %u frame(s) within %d ms (%u timeout(s) "
                            "in the last %lld ms) — render self-pacing until it recovers",
                            consumed, framesPerRender, waitTimeoutMs, waitTimeoutsSinceLog,
                            nowMs - lastWaitTimeoutLogMs);
                    lastWaitTimeoutLogMs = nowMs;
                    waitTimeoutsSinceLog = 0;
                }
                break;  // don't stack N timeouts on a stopped display thread
            }
        }
    }
}

int pattern_controller_request_indicator(Indicator ind) {
    currentIndicator = ind;
    return 0;
}

int pattern_controller_reset_indicator() {
    currentIndicator = Indicator::None;
    return 0;
}

Indicator pattern_controller_get_current_indicator(void) {
    return currentIndicator;
}

Animation pattern_controller_get_current_animation(void) {
    return currentAnimation;
}

int pattern_controller_change_to_animation(Animation animation) {
    // Try to get the next animation
    BaseAnimation *next = getAnimation(animation);

    if (!next) {
        LOG_ERR("Cannot change to animation %d", (size_t)animation);
        return -ENOEXEC;  // Bail early: we failed to get a pointer to our next animation
    }

    // Mark the current animation as inactive, if possible
    BaseAnimation *curr = getAnimation(currentAnimation);
    if (curr) {
        curr->setActive(false);
    }

    next->init();
    next->setActive(true);

    currentAnimation = animation;

    // Push the new id to the BT-free Active Animation binding (Core Config's
    // "Active Animation" characteristic registers the setter). Runs on the
    // caller's thread — BT RX, shell, or this file's thread (see file header).
    ActiveAnimationBinding::setLocalActiveAnimation(animation);

    // No settings write here, deliberately — see the note at the top of the
    // anonymous namespace.
    //
    // To be precise about what was removed and what it cost: the deleted code was
    // persistent_value_registry_mark_dirty() plus request_save(), and request_save()
    // only does k_work_reschedule_for_queue() onto the lowest-priority persist
    // workqueue. It never blocked this caller. The 850-1500 ms of NVS work ran on
    // that workqueue ~1 s later, not here. So this removal is about flash endurance
    // and system-wide QSPI load, NOT about unblocking the calling thread.
    //
    // Do not infer from this comment that a display stall is explained by a settings
    // flush: measured on proto0, a flush produced zero display-thread overruns
    // (issue #311), and the ~1 s freezes tracked separately in issue #312 were traced
    // to something else entirely.
    return 0;
}

int pattern_controller_set_pixel_in_framebuffer(const LedConfig *config, size_t x, size_t y,
                                                size_t bufferId, uint8_t red, uint8_t green,
                                                uint8_t blue) {
    // Scale colors by the current brightness before submitting to the LED controller
    return set_pixel_in_framebuffer(config, x, y, bufferId, ((float)red) * sBrightnessForFrame,
                                    ((float)green) * sBrightnessForFrame,
                                    ((float)blue) * sBrightnessForFrame);
}

#if defined(CONFIG_ANIMATION_GLIM_PLAYER)
#define GLIM_PLAYER_SHELL_SUBCMD \
    , (glim_player, 10,          \
       "Glim Player animation (plays files from /NAND:/glim, see the 'glim' shell command)")
#else
#define GLIM_PLAYER_SHELL_SUBCMD
#endif

#if defined(CONFIG_ANIMATION_MATRIX_CODE)
#define MATRIX_CODE_SHELL_SUBCMD \
    , (matrix_code, 11, "Matrix Code animation (green waterfall dots)")
#else
#define MATRIX_CODE_SHELL_SUBCMD
#endif

#if defined(CONFIG_ANIMATION_TILT)
#define TILT_SHELL_SUBCMD \
    , (tilt, 12, "Tilt animation (hue-shifted rainbow driven by accelerometer)")
#else
#define TILT_SHELL_SUBCMD
#endif

#if defined(CONFIG_ANIMATION_PULSE)
#define PULSE_SHELL_SUBCMD \
    , (pulse, 13, "Pulse animation (whole panel breathes a single configurable color)")
#else
#define PULSE_SHELL_SUBCMD
#endif

#if defined(CONFIG_APP_WASM3_MVP)
#define WASM_MVP_SHELL_SUBCMD \
    , (wasm_mvp, 15, "Wasm3 MVP animation (embedded WebAssembly extension)")
#else
#define WASM_MVP_SHELL_SUBCMD
#endif

#if defined(CONFIG_SHELL)

static int cmd_anim_set(const struct shell *shell, size_t argc, char **argv, void *data) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    Animation animation = static_cast<Animation>(reinterpret_cast<intptr_t>(data));
    int ret = pattern_controller_change_to_animation(animation);
    if (ret) {
        shell_error(shell, "Failed to change animation: %d", ret);
    }
    return ret;
}

static int cmd_anim_get(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    Animation current = pattern_controller_get_current_animation();

    /* Map enum value to a human-readable name for display. */
    const char *name;
    switch (current) {
        case Animation::None:
            name = "none";
            break;
        case Animation::ZigZag:
            name = "zigzag";
            break;
        case Animation::Text:
            name = "text";
            break;
        case Animation::BtAdvertising:
            name = "bt_advertising";
            break;
        case Animation::BtConnecting:
            name = "bt_connecting";
            break;
        case Animation::Rainbow:
            name = "rainbow";
            break;
        case Animation::BtPairing:
            name = "bt_pairing";
            break;
        case Animation::MyEyes:
            name = "my_eyes";
            break;
        case Animation::Beat:
            name = "beat";
            break;
        case Animation::FftBars:
            name = "fft_bars";
            break;
#if defined(CONFIG_ANIMATION_GLIM_PLAYER)
        case Animation::GlimPlayer:
            name = "glim_player";
            break;
#endif
#if defined(CONFIG_ANIMATION_MATRIX_CODE)
        case Animation::MatrixCode:
            name = "matrix_code";
            break;
#endif
#if defined(CONFIG_ANIMATION_TILT)
        case Animation::Tilt:
            name = "tilt";
            break;
#endif
#if defined(CONFIG_ANIMATION_PULSE)
        case Animation::Pulse:
            name = "pulse";
            break;
#endif
#if defined(CONFIG_APP_WASM3_MVP)
        case Animation::WasmMvp:
            name = "wasm_mvp";
            break;
#endif
        default:
            name = "unknown";
            break;
    }

    shell_print(shell, "%s", name);
    return 0;
}

SHELL_SUBCMD_DICT_SET_CREATE(sub_anim_set, cmd_anim_set, (none, 0, "No animation (all LEDs off)"),
                             (zigzag, 1, "ZigZag animation"), (text, 2, "Text animation"),
                             (rainbow, 5, "Rainbow animation"), (my_eyes, 7, "My Eyes animation"),
                             (beat, 8, "Beat animation (per-band flash on beat detection)"),
                             (fft_bars, 9, "FFT Bars animation (live frequency bar graph)")
                                 GLIM_PLAYER_SHELL_SUBCMD
                                 MATRIX_CODE_SHELL_SUBCMD
                                 TILT_SHELL_SUBCMD
                                 PULSE_SHELL_SUBCMD
                                 WASM_MVP_SHELL_SUBCMD);

static int cmd_anim_indicator_clear(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    pattern_controller_reset_indicator();
    return 0;
}

static int cmd_anim_indicator_get(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    const char *name;
    switch (pattern_controller_get_current_indicator()) {
        case Indicator::None:
            name = "none";
            break;
        case Indicator::BtAdvertising:
            name = "bt_advertising";
            break;
        case Indicator::BtConnecting:
            name = "bt_connecting";
            break;
        case Indicator::BtPairing:
            name = "bt_pairing";
            break;
        case Indicator::ExtensionsLoading:
            name = "extensions_loading";
            break;
        default:
            name = "unknown";
            break;
    }

    shell_print(shell, "%s", name);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_anim_indicator,
    SHELL_CMD(clear, NULL, "Clear the active indicator and return to the current animation",
              cmd_anim_indicator_clear),
    SHELL_CMD(get, NULL, "Print the active indicator overlay (none if the animation is visible)",
              cmd_anim_indicator_get),
    SHELL_SUBCMD_SET_END);

#if defined(CONFIG_APP_SHUFFLE)

static int cmd_anim_shuffle_status(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(shell, "shuffle: %s, min: %u s, max: %u s, grace: %u s (max %u s)",
                shuffle_service_get_enabled() ? "on" : "off", shuffle_service_get_min_duration_s(),
                shuffle_service_get_max_duration_s(), CONFIG_APP_SHUFFLE_GRACE_S,
                CONFIG_APP_SHUFFLE_MAX_GRACE_S);
    return 0;
}

static int cmd_anim_shuffle_on(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shuffle_service_set_enabled(true);
    shell_print(shell, "shuffle on");
    return 0;
}

static int cmd_anim_shuffle_off(const struct shell *shell, size_t argc, char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shuffle_service_set_enabled(false);
    shell_print(shell, "shuffle off");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_anim_shuffle,
    SHELL_CMD(status, NULL, "Print shuffle enable/min/max/grace", cmd_anim_shuffle_status),
    SHELL_CMD(on, NULL, "Enable shuffle mode", cmd_anim_shuffle_on),
    SHELL_CMD(off, NULL, "Disable shuffle mode", cmd_anim_shuffle_off), SHELL_SUBCMD_SET_END);

#define ANIM_SHUFFLE_SHELL_CMD \
    SHELL_CMD(shuffle, &sub_anim_shuffle, "Shuffle mode commands (issue #121)", NULL),
#else
#define ANIM_SHUFFLE_SHELL_CMD
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_anim, SHELL_CMD(set, &sub_anim_set, "Switch to a named animation", NULL),
    SHELL_CMD(get, NULL, "Print the current animation name", cmd_anim_get),
    SHELL_CMD(indicator, &sub_anim_indicator, "Indicator commands", NULL),
    ANIM_SHUFFLE_SHELL_CMD SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(anim, &sub_anim, "Animation commands", NULL);

#endif /* CONFIG_SHELL */
