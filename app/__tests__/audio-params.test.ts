/**
 * Unit tests for services/audio-params.ts — the app-side mirror of the firmware's
 * audio_param_table.h, the slider scaling, and the Simple-mode macro mappings.
 *
 * Two properties matter more than the rest and are worth stating up front:
 *
 *  1. Every range here must match fw/src/sound/audio_param_table.h. If they drift, the app
 *     draws a slider that can request values the firmware silently clamps, and the thumb
 *     jumps back under the user's finger for no visible reason.
 *  2. Every macro mapping must have a working inverse. Simple mode places its thumb by
 *     inverting the mapping; a mapping that cannot be inverted would misrepresent the
 *     device's real state rather than admitting it is on a custom value.
 */

import {
    ADAPT_SPEED_PRESETS,
    AUDIO_PARAMS,
    AUDIO_PARAM_ORDER,
    AudioParamKey,
    BEAT_FEEL_PRESETS,
    SENSITIVITY_DEFAULT,
    SENSITIVITY_MAX,
    SENSITIVITY_MIN,
    ZERO_SNAP_POSITION,
    adaptSpeedFromFrames,
    alphaFromSensitivity,
    beatFeelFromFrames,
    decodeParam,
    deltaFromSensitivity,
    encodeParam,
    formatParamValue,
    gateFromNoiseLevel,
    noiseLevelFromGate,
    paramFramesToMs,
    paramToPosition,
    positionToParam,
    resolveAudioParams,
    sensitivityFromAlpha,
    sensitivityFromDelta,
} from "@/services/audio-params";

/**
 * Transcribed from fw/src/sound/audio_param_table.h. Duplicated deliberately: this is the
 * cross-check, so deriving it from the thing under test would defeat the purpose.
 */
const FIRMWARE_TABLE: { key: AudioParamKey; min: number; max: number; def: number; uuidSuffix: string }[] = [
    { key: "fluxGamma", min: 1, max: 100000, def: 1000, uuidSuffix: "0000" },
    { key: "beatFluxFloor", min: 0, max: 1, def: 0.08, uuidSuffix: "0001" },
    { key: "beatAlpha", min: 0.1, max: 20, def: 0.3, uuidSuffix: "0002" },
    { key: "beatRefractoryFrames", min: 0, max: 255, def: 5, uuidSuffix: "0003" },
    { key: "agcTargetLow", min: 0.001, max: 0.1, def: 0.002, uuidSuffix: "0004" },
    { key: "agcTargetHigh", min: 0.02, max: 0.5, def: 0.05, uuidSuffix: "0005" },
    { key: "agcRateLimitFrames", min: 1, max: 100, def: 10, uuidSuffix: "0006" },
    { key: "fftSmoothingCoeff", min: 0, max: 1, def: 0.3, uuidSuffix: "0007" },
    { key: "fftEnergyScale", min: 0.1, max: 1000, def: 20, uuidSuffix: "0008" },
    { key: "agcAttackFrames", min: 1, max: 20, def: 3, uuidSuffix: "0009" },
    { key: "agcReleaseFrames", min: 1, max: 100, def: 15, uuidSuffix: "000a" },
    { key: "agcNoiseGateRms", min: 0, max: 0.02, def: 0.0006, uuidSuffix: "000b" },
    { key: "beatSfDelta", min: 0, max: 2, def: 0.1, uuidSuffix: "000c" },
    { key: "beatThresholdMode", min: 0, max: 1, def: 0, uuidSuffix: "000d" },
];

describe("parameter table integrity", () => {
    it("matches the firmware ranges, defaults and GATT order exactly", () => {
        expect(AUDIO_PARAM_ORDER).toHaveLength(FIRMWARE_TABLE.length);

        FIRMWARE_TABLE.forEach((fw, index) => {
            expect(AUDIO_PARAM_ORDER[index]).toBe(fw.key);

            const spec = AUDIO_PARAMS[fw.key];
            expect(spec.min).toBeCloseTo(fw.min, 10);
            expect(spec.max).toBeCloseTo(fw.max, 10);
            expect(spec.defaultValue).toBeCloseTo(fw.def, 10);
            // Positional UUIDs: index N is the characteristic whose UUID ends ...000N.
            expect(spec.uuid.endsWith(fw.uuidSuffix)).toBe(true);
        });
    });

    it("gives every parameter unique UUIDs, labels and help copy", () => {
        const uuids = new Set<string>();
        const friendly = new Set<string>();

        AUDIO_PARAM_ORDER.forEach(key => {
            const spec = AUDIO_PARAMS[key];
            expect(spec.key).toBe(key);
            expect(uuids.has(spec.uuid)).toBe(false);
            uuids.add(spec.uuid);
            expect(friendly.has(spec.friendlyLabel)).toBe(false);
            friendly.add(spec.friendlyLabel);

            // Help copy is a product surface; an empty string here ships a blank tooltip.
            expect(spec.help.length).toBeGreaterThan(10);
            expect(spec.detail.length).toBeGreaterThan(30);
            expect(spec.firmwareLabel.length).toBeGreaterThan(0);
        });
    });

    it("keeps every default inside its own range", () => {
        AUDIO_PARAM_ORDER.forEach(key => {
            const spec = AUDIO_PARAMS[key];
            expect(spec.defaultValue).toBeGreaterThanOrEqual(spec.min);
            expect(spec.defaultValue).toBeLessThanOrEqual(spec.max);
        });
    });

    it("only marks the noise gate as a Simple-mode control", () => {
        // Simple mode drives everything else through macros; a second directly-exposed
        // parameter there would be a design change, not a tweak.
        const simple = AUDIO_PARAM_ORDER.filter(k => !AUDIO_PARAMS[k].advancedOnly);
        expect(simple).toEqual(["agcNoiseGateRms"]);
    });

    it("gives enum parameters labels covering their whole range", () => {
        AUDIO_PARAM_ORDER.forEach(key => {
            const spec = AUDIO_PARAMS[key];
            if (spec.kind !== "enum") {
                expect(spec.enumLabels).toBeUndefined();
                return;
            }
            expect(spec.enumLabels).toBeDefined();
            expect(spec.enumLabels).toHaveLength(spec.max - spec.min + 1);
        });
    });
});

describe("slider scaling", () => {
    it("round-trips value -> position -> value for every parameter", () => {
        AUDIO_PARAM_ORDER.forEach(key => {
            const spec = AUDIO_PARAMS[key];
            const probes = [
                spec.min,
                spec.defaultValue,
                spec.max,
                spec.min + (spec.max - spec.min) * 0.25,
                spec.min + (spec.max - spec.min) * 0.75,
            ];

            probes.forEach(value => {
                const snapped = positionToParam(spec, paramToPosition(spec, value));
                const again = positionToParam(spec, paramToPosition(spec, snapped));
                // Snapping may move a probe once; it must not keep moving after that.
                expect(again).toBeCloseTo(snapped, 9);
                expect(snapped).toBeGreaterThanOrEqual(spec.min);
                expect(snapped).toBeLessThanOrEqual(spec.max);
            });
        });
    });

    it("puts the ends of the travel exactly on min and max", () => {
        AUDIO_PARAM_ORDER.forEach(key => {
            const spec = AUDIO_PARAMS[key];
            expect(positionToParam(spec, 0)).toBeCloseTo(spec.min, 9);
            expect(positionToParam(spec, 1)).toBeCloseTo(spec.max, 9);
        });
    });

    it("uses log travel for wide-range parameters", () => {
        // Flux gamma spans 1..100000. On a linear slider the default (1000) would sit at 1% of
        // travel, which is the entire reason for log scaling.
        const spec = AUDIO_PARAMS.fluxGamma;
        const pos = paramToPosition(spec, spec.defaultValue);
        expect(pos).toBeGreaterThan(0.5);
        expect(pos).toBeLessThan(0.85);
    });

    it("reserves the bottom of the travel for exactly zero on allowsZero parameters", () => {
        const gate = AUDIO_PARAMS.agcNoiseGateRms;
        expect(gate.allowsZero).toBe(true);

        expect(positionToParam(gate, 0)).toBe(0);
        expect(positionToParam(gate, ZERO_SNAP_POSITION)).toBe(0);
        expect(positionToParam(gate, ZERO_SNAP_POSITION + 0.001)).toBeGreaterThan(0);
        expect(paramToPosition(gate, 0)).toBe(0);
    });

    it("clamps and never returns NaN for hostile input", () => {
        const spec = AUDIO_PARAMS.beatAlpha;
        expect(positionToParam(spec, -5)).toBe(spec.min);
        expect(positionToParam(spec, 99)).toBe(spec.max);
        expect(positionToParam(spec, NaN)).toBe(spec.min);
        expect(paramToPosition(spec, NaN)).toBe(0);
        expect(paramToPosition(spec, Infinity)).toBe(1);
    });

    it("snaps integer parameters to whole frames", () => {
        const spec = AUDIO_PARAMS.agcAttackFrames;
        for (let i = 0; i <= 20; i++) {
            const value = positionToParam(spec, i / 20);
            expect(Number.isInteger(value)).toBe(true);
        }
    });
});

describe("display formatting", () => {
    it("shows frame counts as milliseconds", () => {
        expect(paramFramesToMs(5)).toBe(160);
        expect(formatParamValue(AUDIO_PARAMS.beatRefractoryFrames, 5)).toBe("160 ms");
        expect(formatParamValue(AUDIO_PARAMS.agcReleaseFrames, 15)).toBe("480 ms");
        expect(formatParamValue(AUDIO_PARAMS.agcRateLimitFrames, 10)).toBe("320 ms");
    });

    it("shows the zero label rather than a misleading 0.00000", () => {
        expect(formatParamValue(AUDIO_PARAMS.agcNoiseGateRms, 0)).toBe("Off - never mute");
        expect(formatParamValue(AUDIO_PARAMS.agcNoiseGateRms, 0.0006)).toBe("0.00060");
    });

    it("shows enum labels, not raw numbers", () => {
        expect(formatParamValue(AUDIO_PARAMS.beatThresholdMode, 0)).toBe("Average");
        expect(formatParamValue(AUDIO_PARAMS.beatThresholdMode, 1)).toBe("Median");
    });
});

describe("wire encoding", () => {
    it("round-trips floats and integers through base64", () => {
        AUDIO_PARAM_ORDER.forEach(key => {
            const spec = AUDIO_PARAMS[key];
            const encoded = encodeParam(spec, spec.defaultValue);
            const decoded = decodeParam(spec, encoded);
            expect(decoded).not.toBeNull();
            // float32 has ~7 significant digits, so compare relatively.
            expect(Math.abs((decoded as number) - spec.defaultValue)).toBeLessThan(
                Math.max(Math.abs(spec.defaultValue) * 1e-6, 1e-9),
            );
        });
    });

    it("clamps out-of-range values before they reach the wire", () => {
        const spec = AUDIO_PARAMS.beatAlpha;
        expect(decodeParam(spec, encodeParam(spec, 1e9))).toBeCloseTo(spec.max, 4);
        expect(decodeParam(spec, encodeParam(spec, -1))).toBeCloseTo(spec.min, 4);
    });

    it("returns null rather than NaN for absent or corrupt values", () => {
        const spec = AUDIO_PARAMS.beatAlpha;
        expect(decodeParam(spec, null)).toBeNull();
        expect(decodeParam(spec, undefined)).toBeNull();
        expect(decodeParam(spec, "")).toBeNull();
    });
});

describe("resolveAudioParams", () => {
    const encodedAlpha = encodeParam(AUDIO_PARAMS.beatAlpha, 1.5);

    it("returns only the parameters the device actually exposes", () => {
        const resolved = resolveAudioParams({ [AUDIO_PARAMS.beatAlpha.uuid]: { value: encodedAlpha } });
        expect(resolved).toHaveLength(1);
        expect(resolved[0].spec.key).toBe("beatAlpha");
        expect(resolved[0].value).toBeCloseTo(1.5, 5);
    });

    it("preserves firmware GATT order", () => {
        const chars: Record<string, { value: string | null }> = {};
        AUDIO_PARAM_ORDER.forEach(key => {
            chars[AUDIO_PARAMS[key].uuid] = { value: null };
        });
        const resolved = resolveAudioParams(chars);
        expect(resolved.map(r => r.spec.key)).toEqual(AUDIO_PARAM_ORDER);
    });

    it("reports a null value for a characteristic that has not been read yet", () => {
        const resolved = resolveAudioParams({ [AUDIO_PARAMS.beatAlpha.uuid]: { value: null } });
        expect(resolved[0].value).toBeNull();
    });

    it("lets firmware-supplied overrides win over the built-in table", () => {
        // The forward-compatibility seam: a future ranges characteristic feeds in here.
        const resolved = resolveAudioParams(
            { [AUDIO_PARAMS.beatAlpha.uuid]: { value: encodedAlpha } },
            { beatAlpha: { max: 5 } },
        );
        expect(resolved[0].spec.max).toBe(5);
        expect(resolved[0].spec.min).toBe(AUDIO_PARAMS.beatAlpha.min); // untouched fields survive
    });
});

describe("macro mappings", () => {
    it("anchors every macro midpoint on the firmware default", () => {
        expect(alphaFromSensitivity(SENSITIVITY_DEFAULT)).toBeCloseTo(AUDIO_PARAMS.beatAlpha.defaultValue, 6);
        expect(deltaFromSensitivity(SENSITIVITY_DEFAULT)).toBeCloseTo(AUDIO_PARAMS.beatSfDelta.defaultValue, 6);
        expect(gateFromNoiseLevel(SENSITIVITY_DEFAULT)).toBeCloseTo(AUDIO_PARAMS.agcNoiseGateRms.defaultValue, 8);
    });

    it("inverts exactly for every integer step", () => {
        for (let s = SENSITIVITY_MIN; s <= SENSITIVITY_MAX; s++) {
            expect(sensitivityFromAlpha(alphaFromSensitivity(s))).toBe(s);
            expect(sensitivityFromDelta(deltaFromSensitivity(s))).toBe(s);
            expect(noiseLevelFromGate(gateFromNoiseLevel(s))).toBe(s);
        }
    });

    it("is monotonic: higher sensitivity means a lower threshold", () => {
        for (let s = SENSITIVITY_MIN; s < SENSITIVITY_MAX; s++) {
            expect(alphaFromSensitivity(s + 1)).toBeLessThan(alphaFromSensitivity(s));
            expect(deltaFromSensitivity(s + 1)).toBeLessThan(deltaFromSensitivity(s));
        }
        // The gate runs the other way: a higher setting ignores MORE background noise.
        for (let s = SENSITIVITY_MIN; s < SENSITIVITY_MAX; s++) {
            expect(gateFromNoiseLevel(s + 1)).toBeGreaterThan(gateFromNoiseLevel(s));
        }
    });

    it("stays inside the firmware clamp range at both extremes", () => {
        const alpha = AUDIO_PARAMS.beatAlpha;
        const delta = AUDIO_PARAMS.beatSfDelta;
        const gate = AUDIO_PARAMS.agcNoiseGateRms;

        [SENSITIVITY_MIN, SENSITIVITY_MAX].forEach(s => {
            expect(alphaFromSensitivity(s)).toBeGreaterThanOrEqual(alpha.min);
            expect(alphaFromSensitivity(s)).toBeLessThanOrEqual(alpha.max);
            expect(deltaFromSensitivity(s)).toBeGreaterThanOrEqual(delta.min);
            expect(deltaFromSensitivity(s)).toBeLessThanOrEqual(delta.max);
            expect(gateFromNoiseLevel(s)).toBeGreaterThanOrEqual(gate.min);
            expect(gateFromNoiseLevel(s)).toBeLessThanOrEqual(gate.max);
        });
    });

    it("hits the documented endpoint values", () => {
        expect(alphaFromSensitivity(1)).toBeCloseTo(1.5, 6);
        expect(alphaFromSensitivity(10)).toBeCloseTo(0.1, 6);
        expect(deltaFromSensitivity(1)).toBeCloseTo(0.4, 6);
        expect(deltaFromSensitivity(10)).toBeCloseTo(0.025, 6);
        expect(gateFromNoiseLevel(1)).toBeCloseTo(0.0001, 8);
        expect(gateFromNoiseLevel(10)).toBeCloseTo(0.004, 8);
    });

    it("reports null for a value that is not on any step, so the UI can say Custom", () => {
        // The shared dev board carries a persisted beat_alpha of 1.5 from Phase 1 — which IS
        // step 1 — so pick something genuinely off-grid.
        expect(sensitivityFromAlpha(0.77)).toBeNull();
        expect(sensitivityFromAlpha(19)).toBeNull();
        expect(sensitivityFromDelta(1.9)).toBeNull();
        expect(noiseLevelFromGate(0.0175)).toBeNull();
    });

    it("treats a gate of exactly zero as Off, not as a custom value", () => {
        expect(noiseLevelFromGate(0)).toBe("off");
        expect(gateFromNoiseLevel("off")).toBe(0);
    });

    it("rejects nonsense without throwing", () => {
        expect(sensitivityFromAlpha(NaN)).toBeNull();
        expect(sensitivityFromAlpha(-1)).toBeNull();
        expect(sensitivityFromAlpha(0)).toBeNull();
        expect(noiseLevelFromGate(Infinity)).toBeNull();
    });
});

describe("preset macros", () => {
    it("centres Beat feel and Adapt speed on the firmware defaults", () => {
        expect(beatFeelFromFrames(AUDIO_PARAMS.beatRefractoryFrames.defaultValue)).toBe("Normal");
        expect(
            adaptSpeedFromFrames(
                AUDIO_PARAMS.agcAttackFrames.defaultValue,
                AUDIO_PARAMS.agcReleaseFrames.defaultValue,
                AUDIO_PARAMS.agcRateLimitFrames.defaultValue,
            ),
        ).toBe("Normal");
    });

    it("reports null for combinations that are not a preset", () => {
        expect(beatFeelFromFrames(7)).toBeNull();
        expect(adaptSpeedFromFrames(3, 15, 99)).toBeNull();
    });

    it("keeps every preset inside the firmware clamp ranges", () => {
        BEAT_FEEL_PRESETS.forEach(p => {
            expect(p.refractoryFrames).toBeGreaterThanOrEqual(AUDIO_PARAMS.beatRefractoryFrames.min);
            expect(p.refractoryFrames).toBeLessThanOrEqual(AUDIO_PARAMS.beatRefractoryFrames.max);
            expect(p.blurb.length).toBeGreaterThan(0);
        });
        ADAPT_SPEED_PRESETS.forEach(p => {
            expect(p.attackFrames).toBeGreaterThanOrEqual(AUDIO_PARAMS.agcAttackFrames.min);
            expect(p.attackFrames).toBeLessThanOrEqual(AUDIO_PARAMS.agcAttackFrames.max);
            expect(p.releaseFrames).toBeGreaterThanOrEqual(AUDIO_PARAMS.agcReleaseFrames.min);
            expect(p.releaseFrames).toBeLessThanOrEqual(AUDIO_PARAMS.agcReleaseFrames.max);
            expect(p.rateLimitFrames).toBeGreaterThanOrEqual(AUDIO_PARAMS.agcRateLimitFrames.min);
            expect(p.rateLimitFrames).toBeLessThanOrEqual(AUDIO_PARAMS.agcRateLimitFrames.max);
        });
    });

    it("keeps preset labels unique so a pill group can key on them", () => {
        expect(new Set(BEAT_FEEL_PRESETS.map(p => p.label)).size).toBe(BEAT_FEEL_PRESETS.length);
        expect(new Set(ADAPT_SPEED_PRESETS.map(p => p.label)).size).toBe(ADAPT_SPEED_PRESETS.length);
    });
});
