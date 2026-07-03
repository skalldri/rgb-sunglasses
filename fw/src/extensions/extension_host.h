#pragma once

#include <animations/animation_imu_source.h>
#include <animations/animation_renderer.h>
#include <animations/animation_types.h>
#include <rgbx/rgbx_api.h>

#include <cstddef>
#include <cstdint>

/**
 * extension_host — kernel-side owner of sandboxed LLEXT animation extensions
 * (issue #85). Loads /NAND:/ext/*.llext at boot, validates each manifest
 * against the rgbx ABI (include/rgbx/rgbx_api.h), and executes extension code
 * exclusively on one dedicated K_USER thread confined to the active
 * extension's memory domain.
 *
 * The sandbox thread is recreated on every activation (and after every fault
 * or deadline overrun): the entry/tick function pointers travel as thread
 * arguments, so the user thread needs no access to any kernel-side state —
 * its domain is exactly the extension's four llext partitions plus
 * z_libc_partition.
 */
namespace extension_host {

/** Extension animation IDs occupy 0x20 + slot. The band 0x20..0x7F is
 *  reserved: it stays clear of the built-in Animation enum (0..12) and of
 *  the fixed characteristic UUID groups (0xaaaa/0xbbbb/0xcccc) that
 *  anim_id<<8 in BT_ANIMATION_SERVICE_UUID must avoid. */
inline constexpr uint32_t kAnimationIdBase = 0x20;
inline constexpr size_t kMaxExtensions = 4;
inline constexpr size_t kMaxNameLen = 24;       // matches BtGattString<24> display names
inline constexpr size_t kMaxParamNameLen = 20;  // CUD budget per param

struct ParamInfo {
    char name[kMaxParamNameLen];
    uint8_t type;  // enum rgbx_param_type
    uint32_t defaultValue;
};

/** Discover, load, validate, and register every extension (animation
 *  registry proxy + BLE service). Must run once, from kernel-mode thread
 *  context, after the FAT mount and after
 *  animation_registry_register_defaults(). */
void init();

size_t count();
bool isLoaded(size_t slot);
bool isFaulted(size_t slot);
const char *name(size_t slot);  // display name copied out of the manifest
size_t paramCount(size_t slot);
const ParamInfo *paramInfo(size_t slot, size_t index);
uint32_t paramValue(size_t slot, size_t index);
void setParamValue(size_t slot, size_t index, uint32_t value);

/** Animation id for a slot (kAnimationIdBase + slot). */
Animation animationId(size_t slot);

/** (Re)creates the sandbox thread inside `slot`'s domain and runs the
 *  extension's rgbx_init() there. Clears a previous fault. Called by the
 *  proxy on setActive(true). */
bool activate(size_t slot);

/** Aborts the sandbox thread. Called by the proxy on setActive(false). */
void deactivate(size_t slot);

/** Runs one sandboxed tick: writes the input snapshot into the extension,
 *  signals the sandbox thread, waits up to CONFIG_APP_EXT_TICK_DEADLINE_MS,
 *  then copies the extension's framebuffer into `renderer`. On deadline
 *  overrun or fault the slot is marked faulted, the thread is aborted, and
 *  false is returned (issue #85 recovery path). */
bool tick(size_t slot, uint32_t dtMs, AnimationRenderer &renderer);

/** Optional IMU snapshot provider for rgbx_inputs (zeros when unset). */
void set_imu_source(AnimationImuSource *source);

}  // namespace extension_host

/** Defined in src/imu/animation_adapters/imu_animations_imu.cpp (CONFIG_IMU
 *  builds): wires the shared drain-and-cache IMU source into the host. */
void extension_host_bind_default_imu_dependencies();
