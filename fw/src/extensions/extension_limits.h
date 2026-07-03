#pragma once

#include <animations/animation_types.h>

#include <cstddef>
#include <cstdint>

/**
 * @file
 * @brief Capacity limits shared across the extension subsystem.
 *
 * Single source of truth for every constant that more than one extension
 * module depends on (host, registry, BLE, proxies) so the values cannot
 * drift apart. Dependency-free by design: extension_registry.h must be able
 * to include this without dragging in the animation/BLE headers.
 */
namespace extension_host {

/** @brief Maximum number of extension slots (discovered files, animation
 *  registry entries, BLE services, proxies). extension_registry::kMaxFiles
 *  aliases this. */
inline constexpr size_t kMaxExtensions = 16;

/**
 * @brief First animation ID of the extension band; slot N gets
 * `kAnimationIdBase + N`.
 *
 * Constraints (enforced by the static_asserts below, impossible to violate
 * silently):
 *  - must start above every built-in Animation enum value, with headroom for
 *    the enum to keep growing (0x40 leaves 51 free built-in IDs);
 *  - `anim_id << 8` is embedded in each service UUID
 *    (BT_ANIMATION_SERVICE_UUID, bt_service_cpp.h), so no extension ID may
 *    reach the fixed characteristic UUID groups 0xAA.. (Animation Name),
 *    0xBB.. (Is Active), 0xCC.. (metadata blob).
 */
inline constexpr uint32_t kAnimationIdBase = 0x40;

static_assert(kAnimationIdBase >= static_cast<uint32_t>(Animation::Count),
              "extension animation IDs must start above every built-in Animation");
static_assert(kAnimationIdBase + kMaxExtensions - 1 < 0xAA,
              "extension animation IDs must stay below the fixed characteristic "
              "UUID groups (0xaaaa/0xbbbb/0xcccc) used by BT_ANIMATION_SERVICE_UUID");

/** @brief Extension display-name buffer size (bytes incl. NUL) — the same
 *  budget as every built-in animation's name characteristic. */
inline constexpr size_t kMaxNameLen = kAnimationNameMaxLen;

/** @brief Parameter-name buffer size (bytes incl. NUL). A self-imposed CUD
 *  descriptor budget, NOT a Bluetooth-stack limit — the real wire bound is
 *  the negotiated ATT MTU. Kept small because every param name is stored per
 *  slot and read by the app during discovery. */
inline constexpr size_t kMaxParamNameLen = 20;

}  // namespace extension_host
