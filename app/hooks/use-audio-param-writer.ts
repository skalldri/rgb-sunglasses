/**
 * Write coordination for the Audio Tuning sliders.
 *
 * This exists because of one specific interaction. The 14 audio characteristics are NOT
 * notifiable (see the comment block at the top of fw/src/sound/audio_config.cpp), so
 * `bluetooth-context.tsx` compensates by scheduling read-backs at 150 ms and 1200 ms after every
 * write and compare-and-swapping the result into context. That is what makes a firmware clamp
 * visible in the UI — and it is also what would fight a naively-controlled slider, yanking the
 * thumb out from under a dragging finger.
 *
 * The rules that fall out of that:
 *
 *  1. While the user is dragging, and until the settle window expires, a LOCAL value owns the
 *     thumb and the context value is ignored for rendering. Dragging then feels instant
 *     regardless of BLE latency.
 *  2. Writes are throttled while dragging, with a guaranteed trailing write. Writing during the
 *     drag is intentional — hearing the change live is the whole point of tuning at a venue.
 *  3. The settle window is deliberately longer than the last read-back (1400 > 1200), so the
 *     clamp snap becomes visible exactly once, at a moment the user is looking at it, instead
 *     of mid-drag.
 */

import { useCallback, useEffect, useMemo, useRef, useState } from "react";

import { CLAMP_READBACK_DELAYS_MS } from "@/constants/bluetooth";

/** At most one write per this interval while a slider is being dragged. */
export const WRITE_THROTTLE_MS = 250;

/** Headroom between the last clamp read-back and handing the thumb back to context. */
export const SETTLE_MARGIN_MS = 200;

/**
 * How long the local value keeps ownership of the thumb after the drag ends.
 *
 * DERIVED, not chosen. It must outlast the last clamp read-back or the thumb is handed back to
 * context before the clamped value lands, and the snap appears to happen at random. That used to
 * be a bare 1400 next to a comment saying "currently 1200 ms" — two hand-copied literals in two
 * files, with the real delays declared inside a React function body where nothing could import
 * them. Retuning the read-backs to [150, 2000] would have reintroduced the exact bug this
 * constant exists to prevent, with every test still green.
 */
export const SETTLE_MS = Math.max(...CLAMP_READBACK_DELAYS_MS) + SETTLE_MARGIN_MS;

export interface AudioParamWriteRequest {
    uuid: string;
    encoded: string;
}

export interface UseAudioParamWriterOptions {
    write: (uuid: string, encoded: string) => Promise<boolean>;
    onError?: (uuid: string, error: unknown) => void;
}

export interface AudioParamWriter {
    /** The value to render for `uuid`, preferring a local override while one is active. */
    displayValue: (uuid: string, contextValue: number | null) => number | null;
    /** Called continuously while dragging. Throttled. */
    onSlide: (uuid: string, value: number, encode: (v: number) => string) => void;
    /** Called once when the drag ends. Issues the final write and opens the settle window. */
    onSlideComplete: (uuid: string, value: number, encode: (v: number) => string) => void;
    /** Write immediately with no throttling — for pills, dropdowns and macro buttons. */
    writeNow: (uuid: string, value: number, encode: (v: number) => string) => Promise<boolean>;
    /** True while a local override owns the thumb for `uuid`. */
    isOwned: (uuid: string) => boolean;
}

interface PendingWrite {
    timer: ReturnType<typeof setTimeout> | null;
    lastWriteAt: number;
    lastEncoded: string | null;
    trailing: { value: number; encode: (v: number) => string } | null;
}

export function useAudioParamWriter(options: UseAudioParamWriterOptions): AudioParamWriter {
    // Options can change identity every render (inline lambdas from the screen); keeping them in
    // a ref means none of the callbacks below need them as dependencies, which is what stops the
    // write -> context -> new identity -> effect re-run loop documented in app/CLAUDE.md.
    const optionsRef = useRef(options);
    optionsRef.current = options;

    const [overrides, setOverrides] = useState<Record<string, number>>({});
    const pendingRef = useRef<Record<string, PendingWrite>>({});
    const settleTimersRef = useRef<Record<string, ReturnType<typeof setTimeout>>>({});
    const mountedRef = useRef(true);

    useEffect(() => {
        mountedRef.current = true;
        return () => {
            mountedRef.current = false;
            // Deferred BLE work must never outlive the screen — see the "Deferred callback
            // outliving its context" rule in app/CLAUDE.md.
            Object.values(pendingRef.current).forEach(p => {
                if (p.timer) clearTimeout(p.timer);
            });
            Object.values(settleTimersRef.current).forEach(clearTimeout);
            pendingRef.current = {};
            settleTimersRef.current = {};
        };
    }, []);

    const slotFor = useCallback((uuid: string): PendingWrite => {
        const existing = pendingRef.current[uuid];
        if (existing) return existing;
        const created: PendingWrite = { timer: null, lastWriteAt: 0, lastEncoded: null, trailing: null };
        pendingRef.current[uuid] = created;
        return created;
    }, []);

    const doWrite = useCallback(
        async (uuid: string, encoded: string): Promise<boolean> => {
            const slot = slotFor(uuid);
            const now = Date.now();

            /* Dedupe is TIME-BOUNDED, and that bound is the fix for two separate defects.
             *
             * Sliders emit many valueChange events mapping to the same snapped value, and writing
             * each would burn GATT round-trips for no observable effect — so within one throttle
             * window a repeat is still skipped. Outside it, the same value always writes again.
             *
             * An unbounded latch made a failed value UNWRITABLE FOR THE SESSION: every retry hit
             * this early return and reported success while issuing zero writes. It also swallowed
             * a re-write after the value changed underneath us (a firmware clamp read-back, or a
             * `sound dsp set` on the serial shell), because the latch only tracked what the app
             * last sent, not what the device now holds. */
            if (slot.lastEncoded === encoded && now - slot.lastWriteAt < WRITE_THROTTLE_MS) {
                return true;
            }

            slot.lastEncoded = encoded;
            slot.lastWriteAt = now;

            try {
                const ok = await optionsRef.current.write(uuid, encoded);
                if (!ok) {
                    /* THE PRODUCTION PATH ENDS HERE, NOT IN THE CATCH. writeServiceCharacteristic
                     * never throws — it catches every BLE error and returns false — so the catch
                     * below is dead code for the real writer, and clearing the latch only there
                     * meant it was never cleared in practice. */
                    slot.lastEncoded = null;
                }
                return ok;
            } catch (error) {
                // Kept for writers that DO throw (tests, and any future direct-characteristic path).
                slot.lastEncoded = null;
                optionsRef.current.onError?.(uuid, error);
                return false;
            }
        },
        [slotFor],
    );

    const openSettleWindow = useCallback((uuid: string) => {
        const existing = settleTimersRef.current[uuid];
        if (existing) clearTimeout(existing);

        settleTimersRef.current[uuid] = setTimeout(() => {
            delete settleTimersRef.current[uuid];
            if (!mountedRef.current) return;
            setOverrides(prev => {
                if (!(uuid in prev)) return prev;
                const next = { ...prev };
                delete next[uuid];
                return next;
            });
        }, SETTLE_MS);
    }, []);

    const onSlide = useCallback(
        (uuid: string, value: number, encode: (v: number) => string) => {
            setOverrides(prev => (prev[uuid] === value ? prev : { ...prev, [uuid]: value }));

            const slot = slotFor(uuid);
            const since = Date.now() - slot.lastWriteAt;

            if (since >= WRITE_THROTTLE_MS) {
                void doWrite(uuid, encode(value));
                return;
            }

            // Remember the newest value so the trailing edge writes where the thumb actually
            // ended up, not wherever it happened to be when the window opened.
            slot.trailing = { value, encode };
            if (slot.timer) return;

            slot.timer = setTimeout(() => {
                slot.timer = null;
                const trailing = slot.trailing;
                slot.trailing = null;
                if (trailing && mountedRef.current) {
                    void doWrite(uuid, trailing.encode(trailing.value));
                }
            }, WRITE_THROTTLE_MS - since);
        },
        [doWrite, slotFor],
    );

    const onSlideComplete = useCallback(
        (uuid: string, value: number, encode: (v: number) => string) => {
            const slot = slotFor(uuid);
            // The final position supersedes any queued intermediate write.
            if (slot.timer) {
                clearTimeout(slot.timer);
                slot.timer = null;
            }
            slot.trailing = null;

            setOverrides(prev => (prev[uuid] === value ? prev : { ...prev, [uuid]: value }));
            void doWrite(uuid, encode(value));
            openSettleWindow(uuid);
        },
        [doWrite, openSettleWindow, slotFor],
    );

    const writeNow = useCallback(
        async (uuid: string, value: number, encode: (v: number) => string): Promise<boolean> => {
            setOverrides(prev => ({ ...prev, [uuid]: value }));
            const ok = await doWrite(uuid, encode(value));
            openSettleWindow(uuid);
            return ok;
        },
        [doWrite, openSettleWindow],
    );

    const displayValue = useCallback(
        (uuid: string, contextValue: number | null): number | null =>
            uuid in overrides ? overrides[uuid] : contextValue,
        [overrides],
    );

    const isOwned = useCallback((uuid: string): boolean => uuid in overrides, [overrides]);

    /* Stable identity. Everything downstream depends on the WRAPPER — audio.tsx's valueOf,
     * writeParam and every row handler are built from it — so returning a fresh object literal
     * rebuilt all of them each render and defeated the row memoization this hook exists to make
     * possible. The pieces inside were already stable; only the wrapper was not. */
    return useMemo(
        () => ({ displayValue, onSlide, onSlideComplete, writeNow, isOwned }),
        [displayValue, onSlide, onSlideComplete, writeNow, isOwned],
    );
}
