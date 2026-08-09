#pragma once

/**
 * extension_mgmt — the FILE_MGMT custom SMP command group (design:
 * fw/docs/extension-management.md). Lets the companion app LIST what is in a
 * managed on-device file store (union of disk contents and the boot
 * snapshot) and DELETE files from it over the same SMP transport fs_mgmt
 * uses — capabilities fs_mgmt itself lacks and, being SDK code, cannot be
 * given from this repo.
 *
 * EVERYTHING in this header is an app↔firmware wire compatibility surface
 * (mirrored in app/services/mcumgr.ts) — append-only: never renumber the
 * group, commands, error codes, or map keys.
 *
 * Commands carry a "kind" string selecting the managed store; v1 implements
 * "ext" (/NAND:/ext). "glim" is reserved for GLIM management and answers
 * KIND_UNSUPPORTED until implemented.
 */
namespace extension_mgmt {

/** SMP group id: MGMT_GROUP_ID_PERUSER, the first user-range id. */
inline constexpr int kGroupId = 64;

/** Command ids within the group. */
enum CommandId {
    kCmdList = 0,    // read: {kind, off?} -> {entries: [...], off?}
    kCmdDelete = 1,  // write: {kind, name} -> {}
};

/**
 * Group-specific error codes, reported in the standard SMP `err {group, rc}`
 * map. Append-only.
 */
enum class Error {
    kOk = 0,
    /** The requested kind is not implemented (e.g. "glim" in v1). */
    kKindUnsupported = 1,
    /** Name empty, too long, or resolves outside the kind's directory. */
    kInvalidName = 2,
    /** No such file. */
    kNotFound = 3,
    /** fs_unlink failed for a reason other than -ENOENT. */
    kUnlinkFailed = 4,
    /** Reserved: the file was deleted but post-delete cleanup failed. */
    kCleanupFailed = 5,
};

}  // namespace extension_mgmt
