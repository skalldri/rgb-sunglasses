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
    alphaFromSensitivity,
    deltaFromSensitivity,
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
 * A preset's sensitivity intent, expressed on the shared 1..20 macro scale and emitted as BOTH
 * threshold parameters.
 *
 * WHICH parameter carries "sensitivity" depends on the device's threshold mode: mean-sigma mode
 * reads `beatAlpha` and never looks at `beatSfDelta`; median mode does the opposite
 * (`fw/src/sound/audio_dsp.cpp`). A preset that sets only `beatAlpha` — which all three
 * opinionated built-ins used to — therefore has its headline trait silently do nothing on a
 * median-mode board, while its gate and AGC changes land: the user gets a preset that is half
 * applied and reports success.
 *
 * Setting both is the fix rather than setting `beatThresholdMode`, deliberately. The threshold
 * mode is a shape the user (or the firmware default) chose, and this screen already refuses to
 * overwrite it for the Sensitivity macro — a preset quietly switching detector modes would be a
 * bigger surprise than the one being fixed. The unused one is a single wasted GATT write, and it
 * is already correct if the mode is changed later.
 *
 * The scale position is the authored value because the two curves are independent calibrations
 * of the SAME perceptual scale, so position 4.6 means the same subjective sensitivity in either
 * mode. Deriving both from it is what stops the pair drifting.
 */
function sensitivityValues(s: number): { beatAlpha: number; beatSfDelta: number } {
    return { beatAlpha: alphaFromSensitivity(s), beatSfDelta: deltaFromSensitivity(s) };
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
            // A touch below the default sensitivity: a loud room produces more candidate
            // onsets, not fewer, so the bar for "that was a beat" goes up. (Scale 9.66 -> alpha
            // 0.352, the value this preset has carried since it was a literal. Was 4.6 on the
            // 1..10 scale; recomputed to hold alpha when the scale grew to 1..20 — the delta
            // moved ~2%, accepted, since the curves are independent calibrations of one axis.)
            ...sensitivityValues(9.66),
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
            // More sensitive than default, so a brushed snare in a quiet passage still counts.
            // (Scale 12.8 -> alpha 0.221, the previous literal. Was 6.4 on the 1..10 scale;
            // the high half rescaled proportionally, so both alpha and delta carry over exactly.)
            ...sensitivityValues(12.8),
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
            // Well below default sensitivity — the point is NOT to fire at conversation.
            // (Scale 8.53 -> alpha 0.595, the previous literal. Was 3.3 on the 1..10 scale;
            // recomputed to hold alpha when the scale grew to 1..20 — the delta moved ~10%,
            // in the duller direction, which suits this preset's whole point.)
            ...sensitivityValues(8.53),
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
