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

    useEffect(() => {
        setSaved(loadPresets());
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
     * Write a set of parameters, one at a time, recording an undo entry first.
     *
     * The undo snapshot is taken from the values as they are NOW and covers only the keys about
     * to change — so undoing restores exactly what this change disturbed and nothing else.
     * A parameter whose current value is unknown is skipped rather than guessed at, because
     * writing a guess would be worse than leaving it.
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

            setUndoStack(prev => [{ label, values: before }, ...prev].slice(0, UNDO_DEPTH));
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
    }, [undoStack]);

    const saveCurrentAs = useCallback((name: string, now: number): AudioPreset | null => {
        const preset = presetFromValues(name, valuesRef.current, now);
        if (Object.keys(preset.values).length === 0) return null;

        let next: AudioPreset[] = [];
        setSaved(prev => {
            // Same name overwrites rather than accumulating near-duplicates nobody can tell apart.
            const without = prev.filter(p => p.name !== name);
            next = [...without, preset];
            return next;
        });
        savePresets(next);
        return preset;
    }, []);

    const deletePreset = useCallback((id: string) => {
        let next: AudioPreset[] = [];
        setSaved(prev => {
            next = prev.filter(p => p.id !== id);
            return next;
        });
        savePresets(next);
        setSlotA(prev => (prev === id ? null : prev));
        setSlotB(prev => (prev === id ? null : prev));
    }, []);

    /** Apply whichever of A/B is not currently the closer match — the one-tap compare. */
    const swapAB = useCallback(async (): Promise<ApplyResult | null> => {
        const a = findPreset(slotA);
        const b = findPreset(slotB);
        if (!a || !b) return null;

        // "Which one are we on" is decided by which needs fewer writes to reach; ties go to B so
        // the first tap after assigning both slots always does something visible.
        const target = previewDiff(a).length === 0 ? b : a;
        return applyPreset(target);
    }, [applyPreset, findPreset, previewDiff, slotA, slotB]);

    return {
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
    };
}
