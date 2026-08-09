#pragma once

#include <animations/animation_audio_source.h>
#include <animations/animation_button_source.h>
#include <animations/animation_imu_source.h>
#include <animations/animation_renderer.h>
#include <animations/animation_types.h>
#include <extensions/extension_limits.h>
#include <extensions/extension_manifest.h>
#include <rgbx/rgbx_api.h>

#include <cstddef>
#include <cstdint>

/**
 * @file
 * @brief extension_host — kernel-side owner of sandboxed LLEXT animation
 * extensions (issue #85).
 *
 * Lifecycle (load-on-activate): at boot every .llext file in /NAND:/ext is loaded
 * TRANSIENTLY — its manifest is validated (extension_manifest.h) and copied
 * out — then immediately unloaded, so discovery costs no steady-state llext
 * heap. Only the ACTIVE extension is resident: activation records a pending
 * load and the pattern-controller thread performs the actual llext_load +
 * sandbox bring-up lazily on the first tick() (keeping FAT I/O and ELF
 * relocation off the BLE RX / shell threads that call activate()).
 * Deactivation, faults, and deadline overruns unload the extension again.
 *
 * Extension code executes exclusively on one dedicated K_USER thread
 * confined to a single shared memory domain (re-initialized per activation:
 * z_libc_partition + the extension's four llext partitions). The entry/tick
 * function pointers travel as thread arguments, so the user thread needs no
 * access to any kernel-side state.
 */
namespace extension_host {

/** @brief Copied-out, validated parameter description (see
 *  extension_manifest.h). */
using ParamInfo = extension_manifest::ParamInfo;

/**
 * @brief Discover, validate, and register every extension (animation
 * registry proxy + BLE service). Extensions are NOT left loaded — see the
 * file-top lifecycle comment. Must run once, from kernel-mode thread
 * context, after the FAT mount and after
 * animation_registry_register_defaults().
 */
void init();

/** @brief Number of successfully discovered+registered extension slots. */
size_t count();

/** @brief True if `slot` holds a validated, registered extension (says
 *  nothing about llext residency — only the active extension is resident). */
bool isLoaded(size_t slot);

/** @brief True if `slot`'s sandbox died (fault/hang/load failure) and has
 *  not been reset via clearFault(). */
bool isFaulted(size_t slot);

/**
 * @brief True if `slot` was retired by a runtime file delete (FILE_MGMT
 * DELETE, see extension_mgmt.cpp): its .llext is gone from disk, so
 * re-activation would fail the load-on-activate FAT read. Retired slots
 * reject activate(), are skipped by shuffle, and stay registered (slot
 * numbering and GATT services never change mid-boot) until the next boot's
 * rescan forgets them. NOT cleared by clearFault() — there is no file to
 * come back to.
 */
bool isRetired(size_t slot);

/**
 * @brief Marks `slot` retired (see isRetired()). The caller is responsible
 * for switching away first if the slot is active (the FILE_MGMT DELETE
 * handler routes through pattern_controller_change_to_animation, which
 * un-marks Is Active and notifies the app via the standard path).
 */
void retire(size_t slot);

/** @brief The currently active slot, or -1 (includes pending lazy loads). */
int activeSlot();

/** @brief The .llext file name (not path, not display name) `slot` was
 *  discovered from, or nullptr. */
const char *fileName(size_t slot);

/** @brief Slot whose file name equals `name`, or -1. */
int findSlotByFileName(const char *name);

/**
 * @brief Deletes `slot`'s persisted settings records (params + shuffle flag)
 * and unregisters their registry entries, serialized onto the persistence
 * workqueue (see persistent_value_store::purge_value). Only touches keys this
 * slot actually owns (persistRegistered / shufflePersistRegistered) — a
 * display-name-collision loser owns nothing and purges nothing, so the
 * surviving extension's data is never erased. Blocking; call from a
 * preemptible kernel thread (the SMP workqueue), never the persistence
 * workqueue itself.
 */
void purgePersistence(size_t slot);

/**
 * @brief Shuffle mode's good-switch-point signal for a slot (issue #121): the value the
 * extension's optional rgbx_good_moment export held after its most recent completed
 * tick. True when the slot is invalid, faulted, not yet loaded, or its extension does
 * not export the symbol (absent symbol = every frame is a good moment) — so shuffle can
 * always escape a fault banner or a never-loading extension.
 */
bool atGoodSwitchPoint(size_t slot);

/**
 * @brief Whether shuffle may pick this slot's animation (issue #243) — the extension
 * counterpart of the built-ins' "Include in Shuffle" characteristic, persisted under
 * "ext/<sanitized displayName>/shuffle" (see extension_param_persistence.h). Defaults
 * to true; false for invalid slots. Deliberately NOT cleared by a sandbox fault:
 * unlike params, the flag can't have caused the crash, and fault exclusion is
 * isFaulted()'s job (see the shuffle pool in pattern_controller.cpp).
 */
bool shuffleIncluded(size_t slot);

/**
 * @brief Sets the slot's "include in shuffle" flag and persists it (debounced).
 * No BLE push: the characteristic is not notifiable (Android notification-
 * registration budget) — a connected app sees the change on its next read.
 */
void setShuffleInclude(size_t slot, bool include);

/** @brief Display name copied out of the manifest, or nullptr. */
const char *name(size_t slot);

/** @brief Number of parameters `slot` declares. */
size_t paramCount(size_t slot);

/** @brief Validated description of one parameter, or nullptr. */
const ParamInfo *paramInfo(size_t slot, size_t index);

/** @brief Current scalar (UINT32/COLOR/BOOL) value of a parameter; 0 if out
 *  of range. For STRING params the value is unspecified — use
 *  paramString(). */
uint32_t paramValue(size_t slot, size_t index);

/** @brief Sets a scalar parameter (BOOL values are clamped to 0/1 by the
 *  BLE layer; the shell passes raw values). Serialized against the tick
 *  snapshot. */
void setParamValue(size_t slot, size_t index, uint32_t value);

/** @brief Current value of a STRING parameter (always NUL-terminated), or
 *  "" if `index` is not a string parameter. The pointer refers to host-owned
 *  storage that outlives the call but may change on the next write. */
const char *paramString(size_t slot, size_t index);

/**
 * @brief Writes (part of) a STRING parameter value, mirroring the built-in
 * characteristics' write semantics: data lands at `offset`, the byte after
 * the write is forced to NUL, and writes that would not leave room for the
 * terminator are rejected.
 *
 * @return true on success; false if `index` is not a string parameter or
 *         offset+len >= RGBX_PARAM_STRING_MAX.
 */
bool writeParamString(size_t slot, size_t index, size_t offset, const void *data, size_t len);

/** @brief Animation id for a slot (kAnimationIdBase + slot). */
Animation animationId(size_t slot);

/**
 * @brief Requests activation of `slot`: tears down whatever extension is
 * resident and records a pending load; the pattern-controller thread
 * performs the actual llext load + sandbox bring-up on the next tick().
 * Called by the proxy on setActive(true), typically from the BLE RX or
 * shell thread.
 *
 * A faulted slot is rejected (returns false) — a dead extension must be
 * explicitly reset via clearFault() (shell `ext select`); BLE re-activation
 * never auto-clears. NOTE: because the load is deferred, a bring-up failure
 * (bad file, rgbx_init hang/fault) is reported ASYNCHRONOUSLY — the slot
 * faults on the first tick and the animation's Is Active characteristic
 * notifies false.
 */
bool activate(size_t slot);

/** @brief Aborts the sandbox and unloads the extension if `slot` is the
 *  active one. Called by the proxy on setActive(false). */
void deactivate(size_t slot);

/** @brief Clears a slot's fault flag so activate() will accept it again.
 *  Deliberate developer action (shell `ext select`). */
void clearFault(size_t slot);

/**
 * @brief Runs one sandboxed tick: performs the pending lazy load if this
 * activation hasn't loaded yet, writes the input snapshot (params, strings,
 * IMU/audio/buttons) into the extension, signals the sandbox thread, waits
 * for it to finish, then copies the extension's framebuffer into `renderer`.
 *
 * The wait is budgeted against CONFIG_APP_EXT_TICK_CPU_BUDGET_MS of CPU time
 * consumed by the sandbox thread itself, with
 * CONFIG_APP_EXT_TICK_WALL_BACKSTOP_MS bounding the tail for an extension that
 * blocks rather than spins (issue #276). One deadline is computed per call and
 * shared with the lazy load's rgbx_init handshake. Rationale and bounds:
 * fw/docs/threading.md.
 *
 * On budget overrun, fault, or load failure the slot is marked faulted,
 * the sandbox is torn down and the extension unloaded, the animation's
 * Is Active state is un-marked (notifying the app), and false is returned
 * (issue #85 recovery path — the proxy then renders the fault screen).
 */
bool tick(size_t slot, uint32_t dtMs, AnimationRenderer &renderer);

/** @brief Optional IMU snapshot provider for rgbx_inputs (zeros when
 *  unset). */
void set_imu_source(AnimationImuSource *source);

/** @brief Optional audio snapshot provider for rgbx_inputs (zeros when
 *  unset). */
void set_audio_source(AnimationAudioSource *source);

/** @brief Optional button snapshot provider for rgbx_inputs (zeros when
 *  unset). */
void set_button_source(AnimationButtonSource *source);

}  // namespace extension_host

/** @brief Defined in src/imu/animation_adapters/imu_animations_imu.cpp
 *  (CONFIG_IMU builds): wires the shared drain-and-cache IMU source into the
 *  host. */
void extension_host_bind_default_imu_dependencies();

/** @brief Defined in src/sound/animation_adapters/audio_animations_sound.cpp
 *  (CONFIG_AUDIO builds): wires the shared audio analysis source into the
 *  host. */
void extension_host_bind_default_sound_dependencies();

/** @brief Defined in src/buttons/animation_adapters/button_animation_source.cpp:
 *  wires the always-registered button source into the host. */
void extension_host_bind_default_button_dependencies();
