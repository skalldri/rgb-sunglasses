import { useEffect, useState } from 'react';

import { AppUpdateInfo, checkForAppUpdate } from '@/services/app-update';

// Run the update check at most once per app launch. GitHub's anonymous REST API
// is rate-limited to 60 req/hr per IP, so we cache the result process-wide and
// share a single in-flight request across any components that mount the hook
// (see app/CLAUDE.md — no polling/retry-on-mount without a token).
let cached: AppUpdateInfo | null | undefined;
let inFlight: Promise<AppUpdateInfo | null> | null = null;

function getCheck(): Promise<AppUpdateInfo | null> {
    if (cached !== undefined) return Promise.resolve(cached);
    if (!inFlight) {
        inFlight = checkForAppUpdate()
            .then(result => {
                cached = result;
                return result;
            })
            .catch(() => {
                // Swallow errors for the passive launch check — the dedicated
                // update modal surfaces failures when the user checks explicitly.
                cached = null;
                return null;
            })
            .finally(() => {
                inFlight = null;
            });
    }
    return inFlight;
}

/**
 * Passive, launch-once check for a newer companion-app release. Returns the
 * available update (or null). Safe to mount from multiple screens — the network
 * call fires only once per process.
 */
export function useAppUpdateCheck(): { info: AppUpdateInfo | null; loading: boolean } {
    const [info, setInfo] = useState<AppUpdateInfo | null>(cached ?? null);
    const [loading, setLoading] = useState(cached === undefined);

    useEffect(() => {
        let active = true;
        getCheck().then(result => {
            if (active) {
                setInfo(result);
                setLoading(false);
            }
        });
        return () => {
            active = false;
        };
    }, []);

    return { info, loading };
}
