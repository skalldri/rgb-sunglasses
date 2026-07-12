import { McuMgrClient } from '@/services/mcumgr';
import { useEffect, useState } from 'react';
import { Device } from 'react-native-ble-plx';

export function useMcuMgrClient(device: Device | null) {
    const [client, setClient] = useState<McuMgrClient | null>(null);
    const [isInitializing, setIsInitializing] = useState(true);
    const [error, setError] = useState<string>('');

    useEffect(() => {
        if (!device) {
            setClient(null);
            setIsInitializing(false);
            setError('No device connected');
            return;
        }

        // TypeScript doesn't carry the null-narrowing from the guard above into
        // the nested async closure, so capture the narrowed reference once.
        const connectedDevice = device;
        let mcuClient: McuMgrClient | null = null;
        let mounted = true;

        async function init() {
            try {
                mcuClient = new McuMgrClient(connectedDevice);
                await mcuClient.initialize();
                
                if (mounted) {
                    setClient(mcuClient);
                    setError('');
                }
            } catch (e: unknown) {
                const errorMessage = e instanceof Error ? e.message : String(e);
                if (mounted) {
                    setError(`Failed to initialize: ${errorMessage}`);
                    setClient(null);
                }
            } finally {
                if (mounted) {
                    setIsInitializing(false);
                }
            }
        }

        init();

        return () => {
            mounted = false;
            if (mcuClient) {
                mcuClient.destroy();
            }
        };
    }, [device]);

    return { client, isInitializing, error };
}
