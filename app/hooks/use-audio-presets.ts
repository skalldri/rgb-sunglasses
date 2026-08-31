/**
 * Preset state for the Audio Tuning screen: the saved list, A/B slots, apply, and undo.
 *
 * Apply is deliberately sequential and diff-only. Android permits one outstanding GATT
 * operation, so firing fourteen writes concurrently gets most of them rejected; and writing
 * only what actually differs is what keeps an A/B swap to a few hundred milliseconds — fast
 * enough to hear the difference within a bar, which is the entire point of having A and B.
 */

import { useCallback, useEffect, useMemo, useRef, useState } from "react";

import { AudioParamKey } from "@/services/audio-params";
import { loadPresets, savePresets } from "@/services/audio-preset-store";
import {
    AudioPreset,
    AudioPresetDiffEntry,
    BUILT_IN_PRESETS,
    diffPreset,
    presetFromValues,
} from "@/services/audio-presets";

/** How many bulk changes can be walked back. Deep enough for a tuning session, shallow enough to reason about. */
export const UNDO_DEPTH = 5;

export interface UndoEntry {
    label: string;
    /** The values as they were BEFORE the change, for exactly the keys the change touched. */
    values: Partial<Record<AudioParamKey, number>>;
}

export interface ApplyResult {
    applied: number;
    failed: AudioParamKey[];
}

export interface SavePresetResult {
    preset: AudioPreset;
    /** False when the in-memory list updated but the write to disk did not. */
    persisted: boolean;
    /** True when a preset of the same name was overwritten rather than added. */
    replaced: boolean;
}

/**
 * What a tap on A/B did, as three distinct answers rather than a nullable ApplyResult.
 *
 * `identical` exists because it is REACHABLE and was previously indistinguishable from success:
 * presets are partial maps, so the device can match both slots on every key either expresses, at
 * which point applying either writes nothing. That used to be announced as "Swapped (0 changed)"
 * — a report of success for an action that did nothing and could never do anything until a slot
 * is reassigned.
 */
export type SwapOutcome =
    | { kind: "unassigned" }
    | { kind: "identical" }
    | { kind: "applied"; preset: AudioPreset; result: ApplyResult };

export interface UseAudioPresetsOptions {
    /** Current device values, used for diffing and for capturing undo state. */
    currentValues: Partial<Record<AudioParamKey, number>>;
    /** Writes one parameter. Must resolve false (not throw) on a rejected write. */
    writeParam: (key: AudioParamKey, value: number) => Promise<boolean>;
}

export function useAudioPresets({ currentValues, writeParam }: UseAudioPresetsOptions) {
    const [saved, setSaved] = useState<AudioPreset[]>([]);
    const [slotA, setSlotA] = useState<string | null>(null);
    const [slotB, setSlotB] = useState<string | null>(null);
    const [undoStack, setUndoStack] = useState<UndoEntry[]>([]);
    const [applying, setApplying] = useState(false);

    // Both are read inside async callbacks that must not re-create on every value change —
    // see the effect-dependency rule in app/CLAUDE.md.
    const valuesRef = useRef(currentValues);
    valuesRef.current = currentValues;
    const writeRef = useRef(writeParam);
    writeRef.current = writeParam;

    /* The authoritative list lives in a ref, with React state mirroring it for rendering.
     *
     * Persistence must NOT read the list out of a setState updater: React does not promise to
     * invoke the updater synchronously, so `savePresets` would run before the new list existed
     * and write an empty array. That is not theoretical — it shipped and was caught on a Pixel
     * 9 Pro, which wrote `{"version":1,"presets":[]}` after a save that the UI reported as
     * successful. Jest did not catch it because the test environment flushes updaters
     * synchronously (the class of bug app/CLAUDE.md warns is invisible to mocked tests). */
    const savedRef = useRef<AudioPreset[]>([]);

    const commitSaved = useCallback((next: AudioPreset[]): boolean => {
        savedRef.current = next;
        setSaved(next);
        return savePresets(next);
    }, []);

    useEffect(() => {
        const loaded = loadPresets();
        savedRef.current = loaded;
        setSaved(loaded);
    }, []);

    const allPresets = useMemo(() => [...BUILT_IN_PRESETS, ...saved], [saved]);

    const findPreset = useCallback(
        (id: string | null) => (id ? allPresets.find(p => p.id === id) ?? null : null),
        [allPresets],
    );

    const previewDiff = useCallback(
        (preset: AudioPreset): AudioPresetDiffEntry[] => diffPreset(valuesRef.current, preset),
        [],
    );

    /**
     * The write phase, shared by apply and undo.
     *
     * Both need exactly this — sequential writes, a count, and the keys that failed — and both
     * used to carry their own token-for-token copy of it. The genuine difference between them is
     * only the undo bookkeeping around the loop, so anything added here later (retry,
     * abort-on-disconnect, progress) lands on both paths instead of just one.
     */
    const runWrites = useCallback(
        async (entries: { key: AudioParamKey; to: number }[]): Promise<ApplyResult> => {
            setApplying(true);
            const failed: AudioParamKey[] = [];
            let applied = 0;
            try {
                for (const { key, to } of entries) {
                    const ok = await writeRef.current(key, to);
                    if (ok) applied += 1;
                    else failed.push(key);
                }
            } finally {
                setApplying(false);
            }
            return { applied, failed };
        },
        [],
    );

    /**
     * Write a set of parameters, one at a time, recording an undo entry for what actually landed.
     *
     * The snapshot is taken from the values as they are NOW, before any write moves them, and
     * covers only the keys about to change — so undoing restores exactly what this change
     * disturbed and nothing else. A parameter whose current value is unknown is skipped rather
     * than guessed at, because writing a guess would be worse than leaving it.
     *
     * The entry is pushed AFTER the writes, and only for the keys that succeeded. Pushing it
     * up-front unconditionally meant a failed apply still consumed an undo slot: with
     * UNDO_DEPTH = 5, someone whose device had dropped out of range could flush their genuine
     * pre-experiment snapshot off the stack with five failed Apply taps, then walk back through
     * five entries that restore nothing while the hand-tuned state they wanted is gone for good.
     * An apply that changed nothing now leaves no undo entry at all.
     */
    const applyValues = useCallback(
        async (
            label: string,
            entries: { key: AudioParamKey; to: number }[],
        ): Promise<ApplyResult> => {
            if (entries.length === 0) return { applied: 0, failed: [] };

            const before: Partial<Record<AudioParamKey, number>> = {};
            entries.forEach(({ key }) => {
                const v = valuesRef.current[key];
                if (typeof v === "number") before[key] = v;
            });

            const result = await runWrites(entries);

            const failedKeys = new Set(result.failed);
            const restorable: Partial<Record<AudioParamKey, number>> = {};
            entries.forEach(({ key }) => {
                if (failedKeys.has(key)) return;
                const v = before[key];
                if (typeof v === "number") restorable[key] = v;
            });

            if (Object.keys(restorable).length > 0) {
                setUndoStack(prev => [{ label, values: restorable }, ...prev].slice(0, UNDO_DEPTH));
            }

            return result;
        },
        [runWrites],
    );

    const applyPreset = useCallback(
        async (preset: AudioPreset): Promise<ApplyResult> => {
            const entries = diffPreset(valuesRef.current, preset).map(d => ({ key: d.key, to: d.to }));
            return applyValues(`Applied "${preset.name}"`, entries);
        },
        [applyValues],
    );

    const undo = useCallback(async (): Promise<ApplyResult> => {
        const top = undoStack[0];
        if (!top) return { applied: 0, failed: [] };

        // Pop first: a failed restore should not leave the same entry on top to be retried
        // blindly, because the device state has already partly moved.
        setUndoStack(prev => prev.slice(1));

        const entries = (Object.keys(top.values) as AudioParamKey[]).map(key => ({
            key,
            to: top.values[key] as number,
        }));

        return runWrites(entries);
    }, [runWrites, undoStack]);

    /**
     * Capture the current values as a saved preset.
     *
     * Returns null only when there is genuinely nothing to capture. Otherwise it reports what
     * happened, because both outcomes are ones the user has to be told about:
     *
     *  - `persisted: false` — the list updated in memory but the disk write failed, so the
     *    preset is gone at next launch. This used to be swallowed: `commitSaved` returned the
     *    store's boolean and `saveCurrentAs` dropped it on the floor, so the screen announced a
     *    confident `Saved "X"` for a preset that no longer existed the next time the app opened.
     *  - `replaced: true` — a preset of the same name was overwritten. Overwriting on name is
     *    deliberate (it stops near-duplicates nobody can tell apart accumulating), but doing it
     *    SILENTLY is not: the announcement now says which one happened.
     */
    const saveCurrentAs = useCallback(
        (name: string, now: number): SavePresetResult | null => {
            const preset = presetFromValues(name, valuesRef.current, now);
            if (Object.keys(preset.values).length === 0) return null;

            const without = savedRef.current.filter(p => p.name !== name);
            const replaced = without.length !== savedRef.current.length;
            const persisted = commitSaved([...without, preset]);
            return { preset, persisted, replaced };
        },
        [commitSaved],
    );

    /** Returns false when the removal did not reach disk — the preset returns at next launch. */
    const deletePreset = useCallback(
        (id: string): boolean => {
            const persisted = commitSaved(savedRef.current.filter(p => p.id !== id));
            setSlotA(prev => (prev === id ? null : prev));
            setSlotB(prev => (prev === id ? null : prev));
            return persisted;
        },
        [commitSaved],
    );

    /**
     * The one-tap compare: on A, go to B; otherwise go to A.
     *
     * That rule is chosen, not defaulted. The obvious-sounding alternative — "apply whichever
     * needs fewer writes" — makes the destination depend on how far the device has drifted from
     * each slot, so the same tap goes to different places at different times. Mid-set, in the
     * dark, a compare button has to be predictable above all else: tap, hear A; tap, hear B.
     *
     * (The previous comment here described the fewer-writes rule and a tie going to B. Neither
     * was implemented — `diffPreset(current, b)` was never computed at all, and the neither-matches
     * case went to A. The code was the better behaviour; only the description was wrong.)
     */
    const swapAB = useCallback(async (): Promise<SwapOutcome> => {
        const a = findPreset(slotA);
        const b = findPreset(slotB);
        if (!a || !b) return { kind: "unassigned" };

        const onA = previewDiff(a).length === 0;
        const onB = previewDiff(b).length === 0;
        if (onA && onB) return { kind: "identical" };

        const preset = onA ? b : a;
        return { kind: "applied", preset, result: await applyPreset(preset) };
    }, [applyPreset, findPreset, previewDiff, slotA, slotB]);

    /* Stable identity, for the same reason the writer's is (see use-audio-param-writer.ts).
     *
     * A fresh object literal per render is invisible until something downstream memoises on it:
     * `changeCounts` in audio.tsx depended on this object, so its `useMemo` never once hit and it
     * re-diffed every preset against all 14 parameters on every render — including every frame of
     * every slider drag, and while the sheet that consumes it was closed. Memoising here fixes it
     * for every consumer at once rather than one dependency array at a time. */
    return useMemo(
        () => ({
            builtIns: BUILT_IN_PRESETS,
            saved,
            allPresets,
            slotA,
            slotB,
            setSlotA,
            setSlotB,
            undoStack,
            canUndo: undoStack.length > 0,
            applying,
            previewDiff,
            applyPreset,
            applyValues,
            undo,
            saveCurrentAs,
            deletePreset,
            swapAB,
            findPreset,
        }),
        [
            saved,
            allPresets,
            slotA,
            slotB,
            undoStack,
            applying,
            previewDiff,
            applyPreset,
            applyValues,
            undo,
            saveCurrentAs,
            deletePreset,
            swapAB,
            findPreset,
        ],
    );
}
