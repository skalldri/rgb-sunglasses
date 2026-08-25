/**
 * Audio tuning presets: the model, the built-ins, and the diff that drives A/B compare.
 *
 * The venue case this exists for: someone tuning between sets who wants to bounce between
 * "loud club" and "acoustic" without re-dragging four sliders in the dark — and who wants to
 * get back to where they were when a change turns out worse.
 *
 * A preset is deliberately a PARTIAL map. Built-ins set only the parameters they actually mean
 * to express, so applying "Speech / background" does not silently reset the AGC timings someone
 * tuned by hand. A preset captured from the device (`presetFromValues`) is naturally complete,
 * because at that moment every parameter has a known value.
 *
 * Pure and synchronous — persistence lives in audio-preset-store.ts.
 */

import {
    AUDIO_PARAMS,
    AUDIO_PARAM_ORDER,
    AudioParamKey,
    formatParamValue,
} from "@/services/audio-params";

export interface AudioPreset {
    /** Stable identity. Built-ins use a `builtin:` prefix so they can never collide with saved ones. */
    id: string;
    name: string;
    /** Built-ins live in code, are never persisted, and cannot be deleted or overwritten. */
    builtIn: boolean;
    /** One-line "when to use this", shown under the name. */
    blurb?: string;
    /** Only the parameters this preset expresses an opinion about. */
    values: Partial<Record<AudioParamKey, number>>;
    /** Epoch ms; user-saved presets only. */
    savedAt?: number;
}

export interface AudioPresetDiffEntry {
    key: AudioParamKey;
    from: number | null;
    to: number;
}

/** Every parameter at its firmware default. Values come from the metadata table, never hardcoded. */
function factoryValues(): Record<AudioParamKey, number> {
    const out = {} as Record<AudioParamKey, number>;
    AUDIO_PARAM_ORDER.forEach(key => {
        out[key] = AUDIO_PARAMS[key].defaultValue;
    });
    return out;
}

/**
 * Built-in presets.
 *
 * These are starting points, not answers — the room decides. Each one only sets the parameters
 * it has an opinion about, so it composes with whatever else has been tuned by hand.
 */
export const BUILT_IN_PRESETS: AudioPreset[] = [
    {
        id: "builtin:factory",
        name: "Factory defaults",
        builtIn: true,
        blurb: "What the glasses ship with.",
        values: factoryValues(),
    },
    {
        id: "builtin:loud-club",
        name: "Loud club",
        builtIn: true,
        blurb: "Loud, bass-heavy, fairly constant level.",
        values: {
            // Room is loud and never really quiet, so the gate can sit higher without eating
            // music, and the AGC can afford to duck sooner and recover faster.
            agcNoiseGateRms: 0.0015,
            agcTargetHigh: 0.08,
            agcAttackFrames: 2,
            agcReleaseFrames: 10,
            beatAlpha: 0.35,
            beatRefractoryFrames: 6,
        },
    },
    {
        id: "builtin:acoustic",
        name: "Acoustic / quiet set",
        builtIn: true,
        blurb: "Quiet room, dynamic music, you want it to hear everything.",
        values: {
            // The opposite trade: drop the gate well below the shipped default so quiet passages
            // still register, and let the gain ride up slowly rather than chasing dynamics.
            agcNoiseGateRms: 0.00025,
            agcTargetLow: 0.0012,
            agcReleaseFrames: 20,
            beatAlpha: 0.22,
            beatRefractoryFrames: 5,
        },
    },
    {
        id: "builtin:speech",
        name: "Speech / background",
        builtIn: true,
        blurb: "Deliberately dull - will not strobe at conversation.",
        values: {
            // Not a music setting. Raises the bar for what counts as a beat and enforces a long
            // gap, so talking and clinking glasses do not drive the lights.
            agcNoiseGateRms: 0.0008,
            beatAlpha: 0.6,
            beatRefractoryFrames: 12,
        },
    },
];

/** Capture the device's current values as a new, complete preset. */
export function presetFromValues(
    name: string,
    values: Partial<Record<AudioParamKey, number>>,
    savedAt: number,
    id?: string,
): AudioPreset {
    const captured: Partial<Record<AudioParamKey, number>> = {};
    AUDIO_PARAM_ORDER.forEach(key => {
        const v = values[key];
        if (typeof v === "number" && Number.isFinite(v)) captured[key] = v;
    });
    return { id: id ?? `saved:${savedAt}`, name, builtIn: false, values: captured, savedAt };
}

/**
 * What applying `preset` would actually change.
 *
 * Returned in firmware GATT order, which is also the order the writes are issued in — a stable
 * order makes a partly-failed apply reproducible rather than arbitrary. Parameters the preset
 * has no opinion about, or that already hold the target value, are omitted: that is what keeps
 * an A/B swap down to a handful of writes instead of fourteen.
 */
export function diffPreset(
    current: Partial<Record<AudioParamKey, number>>,
    preset: AudioPreset,
): AudioPresetDiffEntry[] {
    const out: AudioPresetDiffEntry[] = [];

    for (const key of AUDIO_PARAM_ORDER) {
        const to = preset.values[key];
        if (typeof to !== "number") continue;

        const from = current[key];
        if (typeof from === "number" && valuesEqual(key, from, to)) continue;

        out.push({ key, from: typeof from === "number" ? from : null, to });
    }

    return out;
}

/**
 * Whether two values for a parameter are the same as far as the device is concerned.
 *
 * Integers compare exactly. Floats go through a relative tolerance because a value that has
 * round-tripped through float32 over BLE will not be bit-identical to the literal in a preset,
 * and rewriting a parameter that is already correct costs a GATT round-trip for no effect.
 */
export function valuesEqual(key: AudioParamKey, a: number, b: number): boolean {
    if (AUDIO_PARAMS[key].kind !== "float") return Math.round(a) === Math.round(b);
    const scale = Math.max(Math.abs(a), Math.abs(b), 1e-9);
    return Math.abs(a - b) <= scale * 1e-6;
}

/** Human-readable summary of one diff entry, for the confirm/undo UI. */
export function describeDiffEntry(entry: AudioPresetDiffEntry): string {
    const spec = AUDIO_PARAMS[entry.key];
    const from = entry.from === null ? "?" : formatParamValue(spec, entry.from);
    return `${spec.friendlyLabel}: ${from} -> ${formatParamValue(spec, entry.to)}`;
}

/** Default name for a preset saved right now, e.g. "Tuned 21:14". */
export function suggestPresetName(now: Date): string {
    const hh = String(now.getHours()).padStart(2, "0");
    const mm = String(now.getMinutes()).padStart(2, "0");
    return `Tuned ${hh}:${mm}`;
}
