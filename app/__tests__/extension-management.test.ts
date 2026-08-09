import {
    buildExtensionPicker,
    deviceFileState,
    planExtensionManagement,
} from '@/services/extension-management';
import type { ExtensionSyncEntry } from '@/services/extension-sync';
import type { DeviceFileEntry } from '@/services/mcumgr';

function syncEntry(name: string, status: ExtensionSyncEntry['status']): ExtensionSyncEntry {
    return {
        name,
        path: `/NAND:/ext/${name}`,
        status,
        asset: { id: 1, name, browser_download_url: `https://example.test/${name}`, size: 1 } as any,
        expectedSha256: 'a'.repeat(64),
        deviceSha256: status === 'missing' ? null : 'b'.repeat(64),
    };
}

function deviceFile(name: string, overrides: Partial<DeviceFileEntry> = {}): DeviceFileEntry {
    return { name, onDisk: true, loaded: true, ...overrides };
}

describe('deviceFileState', () => {
    it('names the four boot-scoped states', () => {
        expect(deviceFileState(deviceFile('a.llext'))).toBe('installed');
        expect(deviceFileState(deviceFile('a.llext', { loaded: false }))).toBe('pending-restart');
        expect(deviceFileState(deviceFile('a.llext', { onDisk: false }))).toBe('removed');
        expect(deviceFileState(deviceFile('a.llext', { faulted: true }))).toBe('faulted');
    });

    it('ranks removed above faulted for a deleted-but-faulted slot', () => {
        // A retired slot may also carry a stale faulted flag; what the user needs
        // to know is that the file is gone.
        expect(deviceFileState(deviceFile('a.llext', { onDisk: false, faulted: true }))).toBe(
            'removed'
        );
    });
});

describe('planExtensionManagement', () => {
    it('joins released entries with their device rows by file name', () => {
        const plan = planExtensionManagement(
            [syncEntry('plasma.llext', 'up-to-date'), syncEntry('wave.llext', 'missing')],
            [deviceFile('plasma.llext'), deviceFile('hello.llext')]
        );

        expect(plan.listAvailable).toBe(true);
        expect(plan.released.map(r => r.entry.name)).toEqual(['plasma.llext', 'wave.llext']);
        expect(plan.released[0].device?.name).toBe('plasma.llext');
        expect(plan.released[1].device).toBeUndefined();
    });

    it('classifies device files the release does not ship as unmanaged', () => {
        const plan = planExtensionManagement(
            [syncEntry('plasma.llext', 'up-to-date')],
            [
                deviceFile('plasma.llext'),
                deviceFile('hello.llext'),
                deviceFile('ghost.llext', { onDisk: false }),
            ]
        );

        expect(plan.unmanaged.map(u => u.device.name)).toEqual(['hello.llext', 'ghost.llext']);
        expect(plan.unmanaged[0].state).toBe('installed');
        expect(plan.unmanaged[1].state).toBe('removed');
    });

    it('marks the device list unavailable rather than empty on old firmware', () => {
        const plan = planExtensionManagement([syncEntry('plasma.llext', 'outdated')], null);

        expect(plan.listAvailable).toBe(false);
        expect(plan.unmanaged).toEqual([]);
        expect(plan.released[0].device).toBeUndefined();
    });
});

describe('buildExtensionPicker preselection rules', () => {
    it('preselects updates and repairs, never installs', () => {
        const items = buildExtensionPicker(
            planExtensionManagement(
                [
                    syncEntry('current.llext', 'up-to-date'),
                    syncEntry('old.llext', 'outdated'),
                    syncEntry('broken.llext', 'unreadable'),
                    syncEntry('new.llext', 'missing'),
                ],
                []
            )
        );

        expect(items).toEqual([
            expect.objectContaining({ name: 'old.llext', action: 'update', preselected: true }),
            expect.objectContaining({ name: 'broken.llext', action: 'repair', preselected: true }),
            expect.objectContaining({ name: 'new.llext', action: 'install', preselected: false }),
        ]);
        // Up-to-date extensions offer nothing — there is no bulk re-install.
        expect(items.find(i => i.name === 'current.llext')).toBeUndefined();
    });

    it('suggests removal of not-in-release files by highlight, never by pre-tick', () => {
        const items = buildExtensionPicker(
            planExtensionManagement([], [deviceFile('hello.llext', { displayName: 'Hello' })])
        );

        expect(items).toEqual([
            expect.objectContaining({
                name: 'hello.llext',
                action: 'remove',
                preselected: false,
                highlighted: true,
            }),
        ]);
    });

    it('offers nothing for a file already removed this boot', () => {
        // The slot ghost has no file left to delete; the restart the picker
        // precedes already frees it.
        const items = buildExtensionPicker(
            planExtensionManagement([], [deviceFile('hello.llext', { onDisk: false })])
        );
        expect(items).toEqual([]);
    });

    it('builds no remove items when the device list is unavailable', () => {
        const items = buildExtensionPicker(
            planExtensionManagement([syncEntry('old.llext', 'outdated')], null)
        );
        expect(items.map(i => i.action)).toEqual(['update']);
    });
});
