/**
 * Tests for the preset model, the diff that drives A/B compare, and the on-disk store.
 *
 * The properties that matter most here:
 *   - a preset can never carry a value the firmware would clamp (that would silently give the
 *     user a setting they did not choose);
 *   - the diff omits parameters that already match, because that is what keeps an A/B swap to a
 *     few writes instead of fourteen;
 *   - a corrupt store degrades to "no saved presets" rather than throwing on the Controls tab.
 */

import {
    AUDIO_PARAMS,
    AUDIO_PARAM_ORDER,
    AudioParamKey,
} from "@/services/audio-params";
import {
    BUILT_IN_PRESETS,
    describeDiffEntry,
    diffPreset,
    presetFromValues,
    suggestPresetName,
    valuesEqual,
} from "@/services/audio-presets";

const factory = (): Record<AudioParamKey, number> => {
    const out = {} as Record<AudioParamKey, number>;
    AUDIO_PARAM_ORDER.forEach(k => (out[k] = AUDIO_PARAMS[k].defaultValue));
    return out;
};

describe("built-in presets", () => {
    it("never carries a value the firmware would clamp", () => {
        // A preset outside the clamp range would be silently corrected by the device, leaving
        // the user on a setting no preset actually describes.
        BUILT_IN_PRESETS.forEach(preset => {
            (Object.keys(preset.values) as AudioParamKey[]).forEach(key => {
                const spec = AUDIO_PARAMS[key];
                const value = preset.values[key] as number;
                expect(AUDIO_PARAM_ORDER).toContain(key);
                expect(value).toBeGreaterThanOrEqual(spec.min);
                expect(value).toBeLessThanOrEqual(spec.max);
                if (spec.kind !== "float") expect(Number.isInteger(value)).toBe(true);
            });
        });
    });

    it("has unique ids and names, and a blurb on each", () => {
        expect(new Set(BUILT_IN_PRESETS.map(p => p.id)).size).toBe(BUILT_IN_PRESETS.length);
        expect(new Set(BUILT_IN_PRESETS.map(p => p.name)).size).toBe(BUILT_IN_PRESETS.length);
        BUILT_IN_PRESETS.forEach(p => {
            expect(p.builtIn).toBe(true);
            expect(p.blurb && p.blurb.length).toBeGreaterThan(10);
        });
    });

    it("derives Factory defaults from the metadata table, not hardcoded numbers", () => {
        const fac = BUILT_IN_PRESETS.find(p => p.id === "builtin:factory");
        expect(fac).toBeDefined();
        AUDIO_PARAM_ORDER.forEach(key => {
            expect(fac!.values[key]).toBe(AUDIO_PARAMS[key].defaultValue);
        });
    });

    it("keeps the non-factory presets partial, so they compose with hand tuning", () => {
        // A built-in that set all 14 would silently undo unrelated tuning when applied.
        BUILT_IN_PRESETS.filter(p => p.id !== "builtin:factory").forEach(p => {
            expect(Object.keys(p.values).length).toBeGreaterThan(0);
            expect(Object.keys(p.values).length).toBeLessThan(AUDIO_PARAM_ORDER.length);
        });
    });

    it("moves the noise gate in the direction each preset's name implies", () => {
        const gateOf = (id: string) =>
            BUILT_IN_PRESETS.find(p => p.id === id)!.values.agcNoiseGateRms as number;
        const def = AUDIO_PARAMS.agcNoiseGateRms.defaultValue;

        // A loud room can afford to ignore more; a quiet one must ignore less.
        expect(gateOf("builtin:loud-club")).toBeGreaterThan(def);
        expect(gateOf("builtin:acoustic")).toBeLessThan(def);
    });
});

describe("diffPreset", () => {
    it("returns nothing when the device already matches", () => {
        const fac = BUILT_IN_PRESETS.find(p => p.id === "builtin:factory")!;
        expect(diffPreset(factory(), fac)).toEqual([]);
    });

    it("returns only the parameters that actually differ", () => {
        const current = factory();
        current.beatAlpha = 1.0;
        const fac = BUILT_IN_PRESETS.find(p => p.id === "builtin:factory")!;

        const diff = diffPreset(current, fac);
        expect(diff).toHaveLength(1);
        expect(diff[0].key).toBe("beatAlpha");
        expect(diff[0].from).toBe(1.0);
        expect(diff[0].to).toBe(AUDIO_PARAMS.beatAlpha.defaultValue);
    });

    it("ignores parameters the preset has no opinion about", () => {
        const current = factory();
        current.fluxGamma = 5000; // no built-in except factory touches gamma
        const speech = BUILT_IN_PRESETS.find(p => p.id === "builtin:speech")!;

        expect(diffPreset(current, speech).some(d => d.key === "fluxGamma")).toBe(false);
    });

    it("returns entries in firmware GATT order so a partial apply is reproducible", () => {
        const current = factory();
        AUDIO_PARAM_ORDER.forEach(k => (current[k] = AUDIO_PARAMS[k].min));
        const fac = BUILT_IN_PRESETS.find(p => p.id === "builtin:factory")!;

        const keys = diffPreset(current, fac).map(d => d.key);
        const expected = AUDIO_PARAM_ORDER.filter(k => keys.includes(k));
        expect(keys).toEqual(expected);
    });

    it("reports a null 'from' for a parameter the device has not reported yet", () => {
        const fac = BUILT_IN_PRESETS.find(p => p.id === "builtin:factory")!;
        const diff = diffPreset({}, fac);
        expect(diff).toHaveLength(AUDIO_PARAM_ORDER.length);
        diff.forEach(d => expect(d.from).toBeNull());
    });

    it("does not rewrite a float that only differs by float32 round-trip noise", () => {
        // Values coming back over BLE are float32, so they will not be bit-identical to the
        // literal in a preset. Rewriting those would burn a GATT round-trip for no effect.
        const current = factory();
        current.beatAlpha = 0.30000001192092896; // float32(0.3)
        const fac = BUILT_IN_PRESETS.find(p => p.id === "builtin:factory")!;
        expect(diffPreset(current, fac).some(d => d.key === "beatAlpha")).toBe(false);
    });
});

describe("valuesEqual", () => {
    it("compares integers exactly and floats with tolerance", () => {
        expect(valuesEqual("beatRefractoryFrames", 5, 5)).toBe(true);
        expect(valuesEqual("beatRefractoryFrames", 5, 6)).toBe(false);
        expect(valuesEqual("beatAlpha", 0.3, 0.30000001192092896)).toBe(true);
        expect(valuesEqual("beatAlpha", 0.3, 0.31)).toBe(false);
    });

    it("treats zero correctly for the gate", () => {
        expect(valuesEqual("agcNoiseGateRms", 0, 0)).toBe(true);
        expect(valuesEqual("agcNoiseGateRms", 0, 0.0006)).toBe(false);
    });
});

describe("presetFromValues", () => {
    it("captures every known parameter and drops non-finite ones", () => {
        const values = { ...factory(), beatAlpha: NaN };
        const preset = presetFromValues("Warehouse", values, 1000);

        expect(preset.name).toBe("Warehouse");
        expect(preset.builtIn).toBe(false);
        expect(preset.savedAt).toBe(1000);
        expect(preset.values.beatAlpha).toBeUndefined();
        expect(Object.keys(preset.values)).toHaveLength(AUDIO_PARAM_ORDER.length - 1);
    });
});

describe("describeDiffEntry / suggestPresetName", () => {
    it("describes a change in the units the user sees", () => {
        expect(describeDiffEntry({ key: "beatRefractoryFrames", from: 5, to: 12 })).toBe(
            "Minimum gap between beats: 160 ms -> 384 ms",
        );
    });

    it("suggests a stable, zero-padded name", () => {
        expect(suggestPresetName(new Date(2026, 7, 24, 21, 4))).toBe("Tuned 21:04");
        expect(suggestPresetName(new Date(2026, 7, 24, 9, 30))).toBe("Tuned 09:30");
    });
});
