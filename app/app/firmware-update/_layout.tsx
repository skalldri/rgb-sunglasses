import { Colors } from '@/constants/theme';
import { FirmwareUpdateProvider } from '@/context/firmware-update-context';
import { McuMgrClientProvider } from '@/context/mcumgr-client-context';
import { useColorScheme } from '@/hooks/use-color-scheme';
import { Stack } from 'expo-router';
import React from 'react';

/**
 * The firmware-update screens, sharing one MCUmgr client.
 *
 * The provider lives here rather than in each screen because a pushed screen does
 * not unmount the one below it — two screens each calling `useMcuMgrClient` would
 * mean two clients monitoring the same SMP characteristic. See
 * `context/mcumgr-client-context.tsx` for the full reasoning.
 *
 * `FirmwareUpdateProvider` sits inside it and owns the single GitHub release lookup
 * plus the shared "a transfer is in flight" flag — both for the same reason as the
 * client: pushed screens stay mounted, so anything per-screen gets duplicated.
 *
 * Screens render their own in-body header (title + back), so the native header is
 * hidden here — same convention as `(tabs)/device-state/_layout.tsx`, and for the
 * same reason: it keeps screens renderable in unit tests without a navigator context.
 */
export default function FirmwareUpdateLayout() {
    const colorScheme = useColorScheme();
    const palette = colorScheme === 'dark' ? Colors.dark : Colors.light;

    return (
        <McuMgrClientProvider>
            <FirmwareUpdateProvider>
            <Stack
                screenOptions={{
                    headerShown: false,
                    contentStyle: { backgroundColor: palette.background },
                }}>
                <Stack.Screen name="index" />
                <Stack.Screen name="flow" />
                <Stack.Screen name="debug" />
                <Stack.Screen name="extensions" />
            </Stack>
            </FirmwareUpdateProvider>
        </McuMgrClientProvider>
    );
}
