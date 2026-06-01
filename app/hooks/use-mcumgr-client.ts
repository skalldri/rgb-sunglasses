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

        let mcuClient: McuMgrClient | null = null;
        let mounted = true;

        async function init() {
            try {
                mcuClient = new McuMgrClient(device);
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
