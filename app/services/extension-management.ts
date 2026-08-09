/**
 * Extension management: the view-model behind the management screen and the
 * guided flow's extension picker.
 *
 * Combines two independent sources into one truthful picture:
 *  - the release side: `planExtensionSync()`'s digest comparison of every
 *    `.llext` the release ships against the device's copies;
 *  - the device side: the FILE_MGMT LIST union (disk contents ∪ boot slots),
 *    which names files the release does NOT ship — including the divergent
 *    boot-scoped states ("uploaded since boot", "deleted but still loaded").
 *
 * The device side is null when the firmware has no FILE_MGMT group (older
 * builds): the release section still works (install/update is plain fs_mgmt
 * upload), but nothing can be listed or removed, so those affordances hide.
 *
 * Deliberately free of React and BLE plumbing so the decision logic —
 * especially the picker preselection rules — is directly testable.
 */

import { ExtensionSyncEntry } from './extension-sync';
import { DeviceFileEntry } from './mcumgr';

/**
 * Boot-scoped lifecycle state of a device file, derived from LIST's flags.
 * Extensions are read at boot, so disk contents and the running slot set can
 * legitimately diverge until the next restart.
 */
export type DeviceFileState =
    /** On disk and loaded — the normal steady state. */
    | 'installed'
    /** On disk but no boot slot — uploaded since boot, takes effect after restart. */
    | 'pending-restart'
    /** Slot loaded but the file is gone — removed; restart frees the slot. */
    | 'removed'
    /** Loaded but the sandbox faulted; `ext select` or restart recovers. */
    | 'faulted';

export function deviceFileState(entry: DeviceFileEntry): DeviceFileState {
    if (entry.loaded && !entry.onDisk) return 'removed';
    // A retired slot whose file is back on disk is a remove-then-reinstall:
    // the firmware refuses to activate the slot until the next boot rescans,
    // so "installed" would be a lie — the restart caption is the truth.
    // (Current firmware already reports this shape as loaded:false; this
    // branch covers the wire contract for builds that still join them.)
    if (entry.retired) return 'pending-restart';
    if (entry.onDisk && !entry.loaded) return 'pending-restart';
    if (entry.faulted) return 'faulted';
    return 'installed';
}

/** One release-shipped extension, with the device's LIST row when available. */
export interface ReleasedExtensionRow {
    entry: ExtensionSyncEntry;
    /**
     * The device's own view of this file name, or undefined when the device
     * doesn't have it — or when LIST is unavailable (old firmware), in which
     * case NO released row carries one.
     */
    device?: DeviceFileEntry;
}

/** One device file this release does not ship. */
export interface UnmanagedExtensionRow {
    device: DeviceFileEntry;
    state: DeviceFileState;
}

export interface ExtensionManagementPlan {
    released: ReleasedExtensionRow[];
    unmanaged: UnmanagedExtensionRow[];
    /** False when the firmware exposes no FILE_MGMT group — `unmanaged` is then unknowable, not empty. */
    listAvailable: boolean;
    /**
     * False when the release side is UNKNOWN (lookup failed or still running)
     * rather than known-empty. `unmanaged` is forced empty then: with no
     * release to compare against, "not in this release" is unknowable, and
     * computing it from an empty asset list would flag every installed
     * extension for removal after any offline/rate-limited GitHub lookup.
     */
    releaseKnown: boolean;
}

/**
 * Join the release plan with the device's LIST.
 *
 * Everything keys on the bare FILE name — the same identity the firmware uses.
 * Never the manifest display name, which nothing guarantees corresponds — and
 * the name comparison is CASE-INSENSITIVE, because FatFs resolves names
 * case-insensitively: `Plasma.llext` on disk IS the release's `plasma.llext`,
 * and an exact-case join would report the same file as both installed and
 * junk-suggested-for-removal.
 */
export function planExtensionManagement(
    syncEntries: ExtensionSyncEntry[],
    deviceFiles: DeviceFileEntry[] | null,
    releaseKnown: boolean = true
): ExtensionManagementPlan {
    // Dedupe device entries case-insensitively too (first wins) — FatFs can't
    // actually hold two files whose names differ only by case.
    const seenDevice = new Set<string>();
    const dedupedFiles = (deviceFiles ?? []).filter(d => {
        const key = d.name.toLowerCase();
        if (seenDevice.has(key)) return false;
        seenDevice.add(key);
        return true;
    });

    const released = syncEntries.map(entry => ({
        entry,
        device:
            deviceFiles === null
                ? undefined
                : dedupedFiles.find(d => d.name.toLowerCase() === entry.name.toLowerCase()),
    }));

    const releasedNames = new Set(syncEntries.map(e => e.name.toLowerCase()));
    const unmanaged = !releaseKnown
        ? []
        : dedupedFiles
              .filter(d => !releasedNames.has(d.name.toLowerCase()))
              .map(device => ({ device, state: deviceFileState(device) }));

    return { released, unmanaged, listAvailable: deviceFiles !== null, releaseKnown };
}

// ============================================================================
// Guided-flow picker
// ============================================================================

export type ExtensionPickerAction = 'install' | 'update' | 'repair' | 'remove';

/**
 * One checkbox in the guided flow's extension picker.
 *
 * Preselection encodes the product rules (fw/docs/extension-management.md §6):
 * updates and repairs of extensions the user already has are preselected;
 * installing something new is never preselected (install stays a choice); and
 * removal — destructive — is suggested by presence and highlight, never by a
 * pre-ticked box. Nothing is silent: every action the restart will take is a
 * visible, individually toggleable row.
 */
export interface ExtensionPickerItem {
    /** Bare file name — the stable identity for selection state. */
    name: string;
    action: ExtensionPickerAction;
    preselected: boolean;
    /** Highlighted rows are the not-in-this-release files suggested for removal. */
    highlighted: boolean;
    /** Present for install/update/repair — what to upload. */
    syncEntry?: ExtensionSyncEntry;
}

export function buildExtensionPicker(plan: ExtensionManagementPlan): ExtensionPickerItem[] {
    const items: ExtensionPickerItem[] = [];

    for (const row of plan.released) {
        switch (row.entry.status) {
            case 'outdated':
                items.push({
                    name: row.entry.name,
                    action: 'update',
                    preselected: true,
                    highlighted: false,
                    syncEntry: row.entry,
                });
                break;
            case 'unreadable':
                // A half-uploaded file the user meant to have: repairing it is
                // the same "keep what you already chose" rule as an update.
                items.push({
                    name: row.entry.name,
                    action: 'repair',
                    preselected: true,
                    highlighted: false,
                    syncEntry: row.entry,
                });
                break;
            case 'missing':
                items.push({
                    name: row.entry.name,
                    action: 'install',
                    preselected: false,
                    highlighted: false,
                    syncEntry: row.entry,
                });
                break;
            case 'up-to-date':
                break;
        }
    }

    for (const row of plan.unmanaged) {
        // 'removed' rows have no file left to delete — the restart this picker
        // precedes already frees their slot.
        if (row.state === 'removed') continue;
        items.push({
            name: row.device.name,
            action: 'remove',
            preselected: false,
            highlighted: true,
        });
    }

    return items;
}
