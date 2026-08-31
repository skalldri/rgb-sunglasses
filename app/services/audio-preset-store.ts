/**
 * On-device persistence for user-saved audio tuning presets.
 *
 * Uses `expo-file-system`'s synchronous File API against a single small JSON file. No new
 * dependency: expo-file-system is already here and already used by services/firmware-source.ts,
 * and pulling in AsyncStorage for one file that will hold a handful of presets would be a
 * dependency for its own sake.
 *
 * Every entry point is total — a missing, unreadable or corrupt file degrades to "no saved
 * presets" and is logged, never thrown. Losing saved presets is annoying; a crash on the
 * Controls tab because a JSON file got truncated is worse.
 */

import { File, Paths } from "expo-file-system/next";

import { AUDIO_PARAMS, AUDIO_PARAM_ORDER, AudioParamKey } from "@/services/audio-params";
import { AudioPreset } from "@/services/audio-presets";

const FILE_NAME = "audio-presets.json";

/** Bumped only on an incompatible shape change; an unknown version is discarded, not guessed at. */
export const AUDIO_PRESET_STORE_VERSION = 1;

interface StoreShape {
    version: number;
    presets: AudioPreset[];
}

function presetFile(): File {
    return new File(Paths.document, FILE_NAME);
}

/**
 * Validate one entry from disk.
 *
 * Deliberately strict: an unknown parameter key or an out-of-range value would otherwise be
 * written straight back to the device on apply, and the firmware would clamp it into something
 * the user never chose. Unknown keys are dropped and values are clamped to the metadata table.
 */
function sanitizePreset(raw: unknown): AudioPreset | null {
    if (typeof raw !== "object" || raw === null) return null;
    const r = raw as Record<string, unknown>;

    if (typeof r.id !== "string" || r.id.length === 0) return null;
    if (typeof r.name !== "string" || r.name.length === 0) return null;
    if (typeof r.values !== "object" || r.values === null) return null;

    const rawValues = r.values as Record<string, unknown>;
    const values: Partial<Record<AudioParamKey, number>> = {};

    for (const key of AUDIO_PARAM_ORDER) {
        const v = rawValues[key];
        if (typeof v !== "number" || !Number.isFinite(v)) continue;
        const spec = AUDIO_PARAMS[key];
        values[key] = Math.min(Math.max(v, spec.min), spec.max);
    }

    if (Object.keys(values).length === 0) return null;

    return {
        id: r.id,
        name: r.name,
        // Built-ins live in code; anything claiming to be one on disk is a stale or tampered
        // file and is demoted rather than trusted (a "built-in" from disk could not be deleted).
        builtIn: false,
        values,
        savedAt: typeof r.savedAt === "number" ? r.savedAt : undefined,
    };
}

export function loadPresets(): AudioPreset[] {
    try {
        const file = presetFile();
        if (!file.exists) return [];

        const parsed = JSON.parse(file.textSync()) as StoreShape;
        if (parsed?.version !== AUDIO_PRESET_STORE_VERSION || !Array.isArray(parsed.presets)) {
            console.log(
                `Audio preset store: unsupported version ${parsed?.version}, ignoring saved presets`,
            );
            return [];
        }

        return parsed.presets.map(sanitizePreset).filter((p): p is AudioPreset => p !== null);
    } catch (error) {
        console.log("Audio preset store: could not read saved presets:", error);
        return [];
    }
}

/** Returns false when the write failed, so the caller can tell the user rather than assume. */
export function savePresets(presets: AudioPreset[]): boolean {
    try {
        const file = presetFile();
        if (!file.exists) file.create({ intermediates: true });

        const payload: StoreShape = {
            version: AUDIO_PRESET_STORE_VERSION,
            presets: presets.filter(p => !p.builtIn),
        };
        file.write(JSON.stringify(payload));
        return true;
    } catch (error) {
        console.log("Audio preset store: could not save presets:", error);
        return false;
    }
}
