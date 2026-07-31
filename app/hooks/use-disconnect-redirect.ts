import { useBluetooth } from "@/context/bluetooth-context";
import { useFocusEffect, useRouter } from "expo-router";
import { useCallback, useEffect, useRef } from "react";

/**
 * Issue #248: when the connected device goes away, pop the Controls flow back
 * to the Connect tab instead of stranding the user on a chain of per-screen
 * "not connected" fallbacks (Battery detail -> "Back to Controls" -> Controls
 * index -> "Go to Connect" -> Connect).
 *
 * Mounted by every screen in the device-state stack. Two behaviors:
 *
 * - Live disconnect while this screen is visible: pop the device-state stack
 *   to its index (so no stale detail screen lingers for the next visit) and
 *   land on the Connect tab, where the auto-reconnect row / device list is.
 * - Refocusing a stale detail screen that outlived the device (the disconnect
 *   happened while the user was on another tab, so no live redirect fired):
 *   pop to the Controls index, which owns the "Not connected" empty state.
 *   Deliberately NO tab switch in this case -- the user chose to open the
 *   Controls tab; bouncing them straight back to Connect would fight them.
 *
 * The per-screen empty states stay as render-time fallbacks for the frames
 * before navigation lands (and for unit tests, which render screens outside
 * a navigator).
 */
export function useDisconnectRedirect() {
    const { selectedDevice } = useBluetooth();
    const router = useRouter();
    const hasDevice = selectedDevice != null;

    // Render-time sync so the focus callback always sees the current value,
    // even if this screen was frozen/blurred while the device went away.
    const hasDeviceRef = useRef(hasDevice);
    hasDeviceRef.current = hasDevice;
    // Previous value, for edge detection -- updated only by the effect below.
    const hadDeviceRef = useRef(hasDevice);
    const isFocusedRef = useRef(false);

    useFocusEffect(
        useCallback(() => {
            isFocusedRef.current = true;
            if (!hasDeviceRef.current && router.canDismiss()) {
                router.dismissAll();
            }
            return () => {
                isFocusedRef.current = false;
            };
        }, [router])
    );

    useEffect(() => {
        const hadDevice = hadDeviceRef.current;
        hadDeviceRef.current = hasDevice;
        if (hadDevice && !hasDevice && isFocusedRef.current) {
            if (router.canDismiss()) {
                router.dismissAll();
            }
            router.navigate('/(tabs)/bluetooth');
        }
    }, [hasDevice, router]);
}
