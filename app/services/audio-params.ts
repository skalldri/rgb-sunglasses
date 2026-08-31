/**
 * Audio tuning parameter metadata, scaling, and the plain-language "macro" controls.
 *
 * This is the app-side mirror of `fw/src/sound/audio_param_table.h`, which is the firmware's
 * single source of truth for every parameter's default and clamp range. Keeping a copy here is
 * deliberate: the ranges are needed to draw a slider *before* any device is connected, and the
 * firmware exposes them over GATT only as `std::clamp` behaviour — there is no range descriptor
 * on the wire yet.
 *
 * When one does land (a Valid Range descriptor plus a bulk ranges blob — see the plan), it
 * arrives through `resolveAudioParams(..., overrides)`. Nothing else in the app needs to change:
 * the table below becomes the fallback for older firmware rather than the only answer.
 *
 * Everything in this file is pure and synchronous so it can be unit-tested without a device.
 */

import {
    AUDIO_FRAME_MS,
    UUID_AUDIO_AGC_ATTACK_FRAMES,
    UUID_AUDIO_AGC_RATE_LIMIT_FRAMES,
    UUID_AUDIO_AGC_RELEASE_FRAMES,
    UUID_AUDIO_AGC_TARGET_HIGH,
    UUID_AUDIO_AGC_TARGET_LOW,
    UUID_AUDIO_BEAT_ALPHA,
    UUID_AUDIO_BEAT_FLUX_FLOOR,
    UUID_AUDIO_BEAT_REFRACTORY_FRAMES,
    UUID_AUDIO_FFT_ENERGY_SCALE,
    UUID_AUDIO_FFT_SMOOTHING_COEFF,
    UUID_AUDIO_FLUX_GAMMA,
    UUID_AUDIO_NOISE_GATE_RMS,
    UUID_AUDIO_SF_DELTA,
    UUID_AUDIO_THRESHOLD_MODE,
    BLE_GATT_CPF_FORMAT_FLOAT32,
    BLE_GATT_CPF_FORMAT_UINT32,
} from "@/constants/bluetooth";
import {
    decodeFloat32FromBase64,
    decodeUint32FromBase64,
    encodeFloat32ToBase64,
    encodeUint32ToBase64,
} from "@/services/ble-value-codec";

export type AudioParamKey =
    | "fluxGamma"
    | "beatFluxFloor"
    | "beatAlpha"
    | "beatRefractoryFrames"
    | "agcTargetLow"
    | "agcTargetHigh"
    | "agcRateLimitFrames"
    | "fftSmoothingCoeff"
    | "fftEnergyScale"
    | "agcAttackFrames"
    | "agcReleaseFrames"
    | "agcNoiseGateRms"
    | "beatSfDelta"
    | "beatThresholdMode";

export type AudioParamGroup = "agc" | "beat" | "display";

export interface AudioParamSpec {
    key: AudioParamKey;
    uuid: string;
    /** Expected CPF format. A mismatch is logged and the wire format wins. */
    cpfFormat: number;
    kind: "float" | "uint" | "enum";
    /** Firmware CUD label, shown in grey so it greps against `sound dsp params` and the docs. */
    firmwareLabel: string;
    /** What a person who is not the firmware author would call it. */
    friendlyLabel: string;
    group: AudioParamGroup;
    min: number;
    max: number;
    defaultValue: number;
    scale: "linear" | "log";
    /** Linear only: the increment values snap to. */
    step?: number;
    /** Log scale whose `min` is 0: the lower anchor of the non-zero part of the travel. */
    logFloor?: number;
    /** Reserve the bottom of the slider for exactly `min` (which is 0 for these). */
    allowsZero?: boolean;
    zeroLabel?: string;
    /** Frame counts are stored as frames but shown as milliseconds. */
    displayUnit: "ms" | "raw";
    decimals: number;
    enumLabels?: string[];
    /** One sentence, always visible under the control. */
    help: string;
    /** The paragraph behind the "?". */
    detail: string;
    /** Hidden from Simple mode. */
    advancedOnly: boolean;
}

/**
 * Below this fraction of slider travel, an `allowsZero` parameter snaps to exactly 0.
 * A continuous slider cannot reliably land on a single point, and for the noise gate the
 * difference between "0.0001" and "off" is a behaviour change, not a rounding detail.
 */
export const ZERO_SNAP_POSITION = 0.02;

/**
 * How far above the zero-snap band a nonzero sub-log-floor value renders.
 *
 * Small enough to be visually indistinguishable from the band's edge, large enough that
 * positionToParam's `p <= ZERO_SNAP_POSITION` test fails — so a grab-and-release cannot
 * round-trip a live value to "off". See paramToPosition.
 */
export const SUB_FLOOR_NUDGE = 1e-3;

export const AUDIO_PARAMS: Record<AudioParamKey, AudioParamSpec> = {
    fluxGamma: {
        key: "fluxGamma",
        uuid: UUID_AUDIO_FLUX_GAMMA,
        cpfFormat: BLE_GATT_CPF_FORMAT_FLOAT32,
        kind: "float",
        firmwareLabel: "Flux Gamma",
        friendlyLabel: "Bass/treble balance",
        group: "beat",
        min: 1,
        max: 100000,
        defaultValue: 1000,
        scale: "log",
        displayUnit: "raw",
        decimals: 0,
        help: "Controls how much quiet detail counts as a change in the sound.",
        detail:
            "The detector compares each moment to the one before it, on a squashed loudness scale. " +
            "Higher values make quiet changes look bigger, so hi-hats and vocals start counting as " +
            "much as the kick drum. Leave it alone unless the lights only ever follow the bass. " +
            "Default 1000.",
        advancedOnly: true,
    },
    beatFluxFloor: {
        key: "beatFluxFloor",
        uuid: UUID_AUDIO_BEAT_FLUX_FLOOR,
        cpfFormat: BLE_GATT_CPF_FORMAT_FLOAT32,
        kind: "float",
        firmwareLabel: "Beat Flux Floor",
        friendlyLabel: "Minimum beat strength",
        group: "beat",
        min: 0,
        max: 1,
        defaultValue: 0.08,
        scale: "log",
        logFloor: 0.005,
        allowsZero: true,
        zeroLabel: "Off",
        displayUnit: "raw",
        decimals: 3,
        help: "A beat has to be at least this big, no matter what else is going on.",
        detail:
            "The hard floor that stops room noise being read as a beat. Raise it if the lights " +
            "twitch in a quiet room; lower it if soft music is ignored. Above about 0.10 it starts " +
            "eating real beats. Default 0.08.",
        advancedOnly: true,
    },
    beatAlpha: {
        key: "beatAlpha",
        uuid: UUID_AUDIO_BEAT_ALPHA,
        cpfFormat: BLE_GATT_CPF_FORMAT_FLOAT32,
        kind: "float",
        firmwareLabel: "Beat Alpha",
        friendlyLabel: "Sensitivity",
        group: "beat",
        min: 0.1,
        max: 20,
        defaultValue: 0.3,
        scale: "log",
        displayUnit: "raw",
        decimals: 2,
        help: "How far above the recent average a sound has to be to count as a beat.",
        detail:
            "The detector keeps a one-second memory of how much the sound has been changing and " +
            "fires when the current change stands out from it. Lower = more beats. This is what the " +
            'Simple "Sensitivity" slider moves. Default 0.3.',
        advancedOnly: true,
    },
    beatRefractoryFrames: {
        key: "beatRefractoryFrames",
        uuid: UUID_AUDIO_BEAT_REFRACTORY_FRAMES,
        cpfFormat: BLE_GATT_CPF_FORMAT_UINT32,
        kind: "uint",
        firmwareLabel: "Beat Refractory Frames",
        friendlyLabel: "Minimum gap between beats",
        group: "beat",
        min: 0,
        max: 255,
        defaultValue: 5,
        scale: "linear",
        step: 1,
        displayUnit: "ms",
        decimals: 0,
        help: "After a beat, ignore that band for this long.",
        detail:
            "Stops one kick drum being counted two or three times as it rings out. 160 ms is a good " +
            "default; around 380 ms locks onto the kick and ignores snares. One step = 32 ms. " +
            "Default 5 steps.",
        advancedOnly: true,
    },
    agcTargetLow: {
        key: "agcTargetLow",
        uuid: UUID_AUDIO_AGC_TARGET_LOW,
        cpfFormat: BLE_GATT_CPF_FORMAT_FLOAT32,
        kind: "float",
        firmwareLabel: "AGC Target Low",
        friendlyLabel: "Quietest we want",
        group: "agc",
        min: 0.001,
        max: 0.1,
        defaultValue: 0.002,
        scale: "log",
        displayUnit: "raw",
        decimals: 4,
        help: "If the sound stays below this, turn the mic up.",
        detail:
            'With "Loudest we want" this is the band the automatic gain tries to keep the music ' +
            "inside. Drag the green window on the meter instead of typing a number. Default 0.002.",
        advancedOnly: true,
    },
    agcTargetHigh: {
        key: "agcTargetHigh",
        uuid: UUID_AUDIO_AGC_TARGET_HIGH,
        cpfFormat: BLE_GATT_CPF_FORMAT_FLOAT32,
        kind: "float",
        firmwareLabel: "AGC Target High",
        friendlyLabel: "Loudest we want",
        group: "agc",
        min: 0.02,
        max: 0.5,
        defaultValue: 0.05,
        scale: "log",
        displayUnit: "raw",
        decimals: 3,
        help: "If the sound goes above this, turn the mic down.",
        detail:
            "Too low and the mic keeps ducking mid-song; too high and loud passages distort. Keep it " +
            'at least 4x above "Quietest we want" or the gain hunts up and down. Default 0.05.',
        advancedOnly: true,
    },
    agcRateLimitFrames: {
        key: "agcRateLimitFrames",
        uuid: UUID_AUDIO_AGC_RATE_LIMIT_FRAMES,
        cpfFormat: BLE_GATT_CPF_FORMAT_UINT32,
        kind: "uint",
        firmwareLabel: "AGC Rate Limit Frames",
        friendlyLabel: "Minimum time between gain changes",
        group: "agc",
        min: 1,
        max: 100,
        defaultValue: 10,
        scale: "linear",
        step: 1,
        displayUnit: "ms",
        decimals: 0,
        help: "How long to wait after changing the mic level before changing it again.",
        detail:
            "Stops the gain pumping up and down between beats. Default 10 steps, about 320 ms.",
        advancedOnly: true,
    },
    fftSmoothingCoeff: {
        key: "fftSmoothingCoeff",
        uuid: UUID_AUDIO_FFT_SMOOTHING_COEFF,
        cpfFormat: BLE_GATT_CPF_FORMAT_FLOAT32,
        kind: "float",
        firmwareLabel: "FFT Smoothing Coeff",
        friendlyLabel: "Bar smoothing",
        group: "display",
        min: 0,
        max: 1,
        defaultValue: 0.3,
        scale: "linear",
        step: 0.01,
        displayUnit: "raw",
        decimals: 2,
        help: "How smooth the bar display looks. Does not affect beat detection.",
        detail: "0 = frozen, 1 = instant and jittery. Default 0.3.",
        advancedOnly: true,
    },
    fftEnergyScale: {
        key: "fftEnergyScale",
        uuid: UUID_AUDIO_FFT_ENERGY_SCALE,
        cpfFormat: BLE_GATT_CPF_FORMAT_FLOAT32,
        kind: "float",
        firmwareLabel: "FFT Energy Scale",
        friendlyLabel: "Bar height",
        group: "display",
        min: 0.1,
        max: 1000,
        defaultValue: 20,
        scale: "log",
        displayUnit: "raw",
        decimals: 1,
        help: "How tall the bars are drawn. Does not affect beat detection.",
        detail:
            "Purely cosmetic - it scales how tall the bars are drawn from the same measurements, " +
            "and changes nothing about how beats are detected. Raise it if the bars look flat, " +
            "lower it if they are always pinned at the top. Default 20.",
        advancedOnly: true,
    },
    agcAttackFrames: {
        key: "agcAttackFrames",
        uuid: UUID_AUDIO_AGC_ATTACK_FRAMES,
        cpfFormat: BLE_GATT_CPF_FORMAT_UINT32,
        kind: "uint",
        firmwareLabel: "AGC Attack Frames",
        friendlyLabel: "How quickly it turns down",
        group: "agc",
        min: 1,
        max: 20,
        defaultValue: 3,
        scale: "linear",
        step: 1,
        displayUnit: "ms",
        decimals: 0,
        help: "How many loud moments in a row before the mic is turned down.",
        detail:
            "Small = reacts fast to a sudden loud passage; large = rides over brief peaks. " +
            "Default 3, about 96 ms.",
        advancedOnly: true,
    },
    agcReleaseFrames: {
        key: "agcReleaseFrames",
        uuid: UUID_AUDIO_AGC_RELEASE_FRAMES,
        cpfFormat: BLE_GATT_CPF_FORMAT_UINT32,
        kind: "uint",
        firmwareLabel: "AGC Release Frames",
        friendlyLabel: "How quickly it turns up",
        group: "agc",
        min: 1,
        max: 100,
        defaultValue: 15,
        scale: "linear",
        step: 1,
        displayUnit: "ms",
        decimals: 0,
        help: "How many quiet moments in a row before the mic is turned up.",
        detail:
            "Deliberately slower than turning down, so one quiet bar does not crank the gain. " +
            "Default 15, about 480 ms.",
        advancedOnly: true,
    },
    agcNoiseGateRms: {
        key: "agcNoiseGateRms",
        uuid: UUID_AUDIO_NOISE_GATE_RMS,
        cpfFormat: BLE_GATT_CPF_FORMAT_FLOAT32,
        kind: "float",
        firmwareLabel: "AGC Noise Gate RMS",
        friendlyLabel: "Ignore background noise",
        group: "agc",
        min: 0,
        max: 0.02,
        defaultValue: 0.0006,
        scale: "log",
        logFloor: 0.0001,
        allowsZero: true,
        zeroLabel: "Off - never mute",
        displayUnit: "raw",
        decimals: 5,
        help:
            "Anything quieter than this counts as silence: the lights stop reacting and the mic " +
            "level is held.",
        detail:
            "The most important knob in a difficult room. Too high and quiet music is ignored " +
            "completely - the giveaway is that turning the music up appears to fix it. Too low and " +
            "the air conditioning becomes a drum beat. Set it to Off to never mute. Default 0.0006.",
        advancedOnly: false,
    },
    beatSfDelta: {
        key: "beatSfDelta",
        uuid: UUID_AUDIO_SF_DELTA,
        cpfFormat: BLE_GATT_CPF_FORMAT_FLOAT32,
        kind: "float",
        firmwareLabel: "Beat SF Delta",
        friendlyLabel: "Sensitivity (median mode)",
        group: "beat",
        min: 0,
        max: 2,
        defaultValue: 0.1,
        scale: "log",
        logFloor: 0.005,
        allowsZero: true,
        zeroLabel: "Off",
        displayUnit: "raw",
        decimals: 3,
        help: "How far above the recent typical level a sound must be - median mode only.",
        detail:
            "Only used when Threshold shape is Median. It is a fixed amount rather than a multiple, " +
            "and the four frequency bands differ in level by more than 20x, so one value cannot suit " +
            "them all. That is why Average ships as the default. Default 0.10.",
        advancedOnly: true,
    },
    beatThresholdMode: {
        key: "beatThresholdMode",
        uuid: UUID_AUDIO_THRESHOLD_MODE,
        cpfFormat: BLE_GATT_CPF_FORMAT_UINT32,
        kind: "enum",
        firmwareLabel: "Beat Threshold Mode",
        friendlyLabel: "Threshold shape",
        group: "beat",
        min: 0,
        max: 1,
        defaultValue: 0,
        scale: "linear",
        step: 1,
        displayUnit: "raw",
        decimals: 0,
        enumLabels: ["Average", "Median"],
        help: "Two ways of deciding what counts as louder than usual.",
        detail:
            "Average (default, recommended) scales itself to each frequency band automatically. " +
            "Median ignores the beats themselves when working out what is normal - better in theory, " +
            "measured worse on this hardware. Try it only if Average will not behave.",
        advancedOnly: true,
    },
};

/**
 * Firmware GATT declaration order. This is also the positional characteristic index order, so
 * it must match the BtGattServer argument list in fw/src/sound/audio_config.cpp.
 */
export const AUDIO_PARAM_ORDER: AudioParamKey[] = [
    "fluxGamma",
    "beatFluxFloor",
    "beatAlpha",
    "beatRefractoryFrames",
    "agcTargetLow",
    "agcTargetHigh",
    "agcRateLimitFrames",
    "fftSmoothingCoeff",
    "fftEnergyScale",
    "agcAttackFrames",
    "agcReleaseFrames",
    "agcNoiseGateRms",
    "beatSfDelta",
    "beatThresholdMode",
];

/* ------------------------------------------------------------------------------------------
 * Slider scaling
 *
 * `position` is always 0..1 of slider travel. Most of these parameters span two to five orders
 * of magnitude (flux gamma is 1..100000), so a linear slider would put every useful value in
 * the bottom 1% of the track. Log scaling is what makes them tunable with a thumb.
 * ---------------------------------------------------------------------------------------- */

export function clampNumber(v: number, lo: number, hi: number): number {
    // NaN has no position on the scale, so it takes the low end. The infinities DO have an
    // obvious position and must saturate towards it — sending +Infinity to `lo` would park a
    // slider at the wrong end of its travel, which is worse than any rounding error.
    if (Number.isNaN(v)) return lo;
    return v < lo ? lo : v > hi ? hi : v;
}

/** Snap to the spec's step grid, anchored at `min`. */
function snapToStep(spec: AudioParamSpec, value: number): number {
    if (!spec.step || spec.step <= 0) return value;
    const steps = Math.round((value - spec.min) / spec.step);
    const snapped = spec.min + steps * spec.step;
    // Re-round to kill float drift like 0.30000000000000004 from repeated addition.
    const decimals = spec.step < 1 ? Math.ceil(-Math.log10(spec.step)) : 0;
    return parseFloat(snapped.toFixed(decimals));
}

/** The lower anchor of the logarithmic part of the travel. */
function logLow(spec: AudioParamSpec): number {
    return spec.allowsZero ? (spec.logFloor ?? spec.max / 1e4) : spec.min;
}

export function positionToParam(spec: AudioParamSpec, position: number): number {
    const p = clampNumber(position, 0, 1);

    if (spec.scale === "linear") {
        const raw = spec.min + p * (spec.max - spec.min);
        const snapped = snapToStep(spec, raw);
        return clampNumber(spec.kind === "float" ? snapped : Math.round(snapped), spec.min, spec.max);
    }

    if (spec.allowsZero && p <= ZERO_SNAP_POSITION) {
        return spec.min; // 0 for every allowsZero parameter
    }

    const lo = logLow(spec);
    const t = spec.allowsZero ? (p - ZERO_SNAP_POSITION) / (1 - ZERO_SNAP_POSITION) : p;
    const value = lo * Math.pow(spec.max / lo, t);
    return clampNumber(spec.kind === "float" ? value : Math.round(value), spec.min, spec.max);
}

export function paramToPosition(spec: AudioParamSpec, value: number): number {
    const v = clampNumber(value, spec.min, spec.max);

    if (spec.scale === "linear") {
        if (spec.max === spec.min) return 0;
        return clampNumber((v - spec.min) / (spec.max - spec.min), 0, 1);
    }

    const lo = logLow(spec);
    if (spec.allowsZero && v <= lo) {
        if (v <= spec.min) return 0; // genuinely off

        /* A NONZERO value below the log floor renders just ABOVE the snap band, never on its
         * edge, so that turning the parameter off always requires deliberately dragging into
         * the band.
         *
         * Sitting exactly on ZERO_SNAP_POSITION made the two directions disagree: this returned
         * the band's edge, while positionToParam maps the whole band (p <= ZERO_SNAP_POSITION)
         * to spec.min. onSlidingComplete fires on touch-up whether or not the thumb moved — and
         * Android's SeekBar tap-jumps to the touch x — so the most natural gesture, grab and
         * release, round-tripped a small live value to 0 and silently disabled the gate.
         *
         * The app cannot itself produce a sub-floor nonzero value, so the exposed case is a
         * board tuned over the serial shell or by another GATT client: precisely the boards
         * whose settings a user would least expect the app to quietly discard. */
        return ZERO_SNAP_POSITION + SUB_FLOOR_NUDGE;
    }

    const t = Math.log(v / lo) / Math.log(spec.max / lo);
    return clampNumber(spec.allowsZero ? ZERO_SNAP_POSITION + t * (1 - ZERO_SNAP_POSITION) : t, 0, 1);
}

/* ------------------------------------------------------------------------------------------
 * Display
 * ---------------------------------------------------------------------------------------- */

export function paramFramesToMs(frames: number): number {
    return Math.round(frames * AUDIO_FRAME_MS);
}

export function formatParamValue(spec: AudioParamSpec, value: number): string {
    if (spec.kind === "enum") {
        const labels = spec.enumLabels ?? [];
        return labels[Math.round(value)] ?? String(value);
    }
    if (spec.allowsZero && value <= spec.min) {
        return spec.zeroLabel ?? "Off";
    }
    if (spec.displayUnit === "ms") {
        return `${paramFramesToMs(value)} ms`;
    }
    return value.toFixed(spec.decimals);
}

/* ------------------------------------------------------------------------------------------
 * Wire encoding
 * ---------------------------------------------------------------------------------------- */

export function encodeParam(spec: AudioParamSpec, value: number): string {
    const clamped = clampNumber(value, spec.min, spec.max);
    return spec.kind === "float"
        ? encodeFloat32ToBase64(clamped)
        : encodeUint32ToBase64(Math.round(clamped));
}

export function decodeParam(spec: AudioParamSpec, encoded?: string | null): number | null {
    if (!encoded) return null;
    try {
        const raw = spec.kind === "float"
            ? decodeFloat32FromBase64(encoded)
            : decodeUint32FromBase64(encoded);
        return Number.isFinite(raw) ? raw : null;
    } catch {
        return null;
    }
}

/* ------------------------------------------------------------------------------------------
 * Resolution against a connected device
 * ---------------------------------------------------------------------------------------- */

export interface ResolvedAudioParam {
    spec: AudioParamSpec;
    /** Decoded current value, or null when the characteristic has not been read yet. */
    value: number | null;
    /** Present only when the device actually exposes this characteristic. */
    encodedValue: string | null;
}

/**
 * Intersect the metadata table with what this device actually exposes.
 *
 * `overrides` is the forward-compatibility seam: when firmware grows a ranges characteristic,
 * the connection hook decodes it and passes the per-parameter min/max/step/enum labels here.
 * Until then it is always undefined and the table above is authoritative. Parameters the device
 * does not expose are omitted entirely rather than rendered as dead controls.
 */
export function resolveAudioParams(
    charsByUuid: Record<string, { value?: string | null } | undefined>,
    overrides?: Partial<Record<AudioParamKey, Partial<AudioParamSpec>>>,
): ResolvedAudioParam[] {
    const resolved: ResolvedAudioParam[] = [];

    for (const key of AUDIO_PARAM_ORDER) {
        const base = AUDIO_PARAMS[key];
        const info = charsByUuid[base.uuid];
        if (!info) continue;

        const spec: AudioParamSpec = overrides?.[key] ? { ...base, ...overrides[key] } : base;
        const encodedValue = info.value ?? null;
        resolved.push({ spec, value: decodeParam(spec, encodedValue), encodedValue });
    }

    return resolved;
}

/* ------------------------------------------------------------------------------------------
 * The Simple-mode macro controls.
 *
 * Each mapping is anchored so that its midpoint is exactly the firmware default, and each has
 * an exact inverse — so opening the screen on a device with arbitrary stored values either
 * places the thumb correctly or reports `null`, which the UI renders as a "Custom" chip. A
 * mapping without a working inverse would silently misrepresent the device's real state.
 * ---------------------------------------------------------------------------------------- */

export const SENSITIVITY_MIN = 1;
export const SENSITIVITY_MAX = 10;
export const SENSITIVITY_DEFAULT = 5;

/** Relative tolerance when inverting a macro mapping (floats round-trip through IEEE-754). */
const MACRO_TOLERANCE = 0.02;

/**
 * Two-sided geometric interpolation anchored at the firmware default.
 *
 * `lowFactor` is what the value is multiplied by at step 1; `highFactor` is what it is
 * multiplied by at step 10. Both are plain multipliers so a mapping that runs "downwards"
 * (alpha: step 1 is the least sensitive, so the biggest alpha) and one that runs "upwards"
 * (the noise gate: step 1 ignores the least) read the same way at the call site.
 */
function twoSidedGeometric(s: number, mid: number, lowFactor: number, highFactor: number): number {
    return s <= SENSITIVITY_DEFAULT
        ? mid * Math.pow(lowFactor, (SENSITIVITY_DEFAULT - s) / (SENSITIVITY_DEFAULT - SENSITIVITY_MIN))
        : mid * Math.pow(highFactor, (s - SENSITIVITY_DEFAULT) / (SENSITIVITY_MAX - SENSITIVITY_DEFAULT));
}

/**
 * Invert `twoSidedGeometric` by solving each branch and keeping the solution that lands inside
 * its own half of the range and reconstructs the input.
 *
 * Solving both branches rather than branching on `value >= mid` is deliberate: that shortcut
 * only works for mappings that decrease with s, and it silently produced garbage for the noise
 * gate, which increases. Checking the reconstruction also gives the "Custom" answer for free —
 * a value that is not on any step reconstructs to something else and is correctly rejected.
 */
/**
 * The unrounded scale position(s) that could produce `value` — one per branch of the curve.
 *
 * Split out because two callers need it at different precisions: the macro inverse below rounds
 * to a step and rejects anything that does not reconstruct (that is how "Custom" is detected),
 * while `macroScaleOf` keeps the continuous answer.
 */
function twoSidedGeometricCandidates(
    value: number,
    mid: number,
    lowFactor: number,
    highFactor: number,
): number[] {
    if (!Number.isFinite(value) || value <= 0 || mid <= 0) return [];

    const lnRatio = Math.log(value / mid);
    const candidates: number[] = [];

    if (lowFactor > 0 && lowFactor !== 1) {
        candidates.push(
            SENSITIVITY_DEFAULT -
                ((SENSITIVITY_DEFAULT - SENSITIVITY_MIN) * lnRatio) / Math.log(lowFactor),
        );
    }
    if (highFactor > 0 && highFactor !== 1) {
        candidates.push(
            SENSITIVITY_DEFAULT +
                ((SENSITIVITY_MAX - SENSITIVITY_DEFAULT) * lnRatio) / Math.log(highFactor),
        );
    }
    return candidates;
}

function invertTwoSidedGeometric(
    value: number,
    mid: number,
    lowFactor: number,
    highFactor: number,
): number | null {
    for (const candidate of twoSidedGeometricCandidates(value, mid, lowFactor, highFactor)) {
        const rounded = Math.round(candidate);
        if (rounded < SENSITIVITY_MIN || rounded > SENSITIVITY_MAX) continue;

        const reconstructed = twoSidedGeometric(rounded, mid, lowFactor, highFactor);
        if (Math.abs(reconstructed - value) <= MACRO_TOLERANCE * Math.max(Math.abs(value), 1e-12)) {
            return rounded;
        }
    }

    return null;
}

/**
 * Macro outputs are clamped to the parameter's own range before they are ever encoded.
 *
 * Without this, `alphaFromSensitivity(10)` evaluates to 0.09999999999999999 — a hair below the
 * firmware's 0.1 minimum — which the device would clamp on read and write back, making the
 * thumb twitch after a write for no reason a user could ever explain.
 */
export function clampToSpec(key: AudioParamKey, value: number): number {
    const spec = AUDIO_PARAMS[key];
    return clampNumber(value, spec.min, spec.max);
}

/**
 * One descriptor per macro, consumed by BOTH directions.
 *
 * The midpoint is READ FROM THE TABLE rather than restated, and the factors are written once
 * rather than once per direction. Previously each macro carried its midpoint and both factors
 * twice — six literal sites across three macros — none of them derived from the spec three
 * screens up that holds the same number. Retuning a default in the table without editing every
 * literal made forward and inverse disagree, and a device sitting on what used to be a macro
 * step then rendered "Custom" everywhere. The round-trip tests would have caught drift WITHIN a
 * pair but not between the table and the macros, which is the drift that could actually happen.
 */
interface MacroCurve {
    key: AudioParamKey;
    lowFactor: number;
    highFactor: number;
}

/** Midpoint always comes from the parameter's own default — never restated here. */
function macroMid(curve: MacroCurve): number {
    return AUDIO_PARAMS[curve.key].defaultValue;
}

function macroForward(curve: MacroCurve, s: number): number {
    const raw = twoSidedGeometric(
        clampNumber(s, SENSITIVITY_MIN, SENSITIVITY_MAX),
        macroMid(curve),
        curve.lowFactor,
        curve.highFactor,
    );
    return clampToSpec(curve.key, raw);
}

function macroInverse(curve: MacroCurve, value: number): number | null {
    return invertTwoSidedGeometric(value, macroMid(curve), curve.lowFactor, curve.highFactor);
}

/** 5 = the firmware default (0.30), 1 = 1.50, 10 = 0.10. */
const ALPHA_CURVE: MacroCurve = { key: "beatAlpha", lowFactor: 5, highFactor: 1 / 3 };
/** Median mode. 5 = the firmware default (0.10), 1 = 0.40, 10 = 0.025. */
const DELTA_CURVE: MacroCurve = { key: "beatSfDelta", lowFactor: 4, highFactor: 1 / 4 };

export function alphaFromSensitivity(s: number): number {
    return macroForward(ALPHA_CURVE, s);
}
export function sensitivityFromAlpha(alpha: number): number | null {
    return macroInverse(ALPHA_CURVE, alpha);
}

export function deltaFromSensitivity(s: number): number {
    return macroForward(DELTA_CURVE, s);
}
export function sensitivityFromDelta(delta: number): number | null {
    return macroInverse(DELTA_CURVE, delta);
}

/**
 * "Ignore background noise": Off, or 1..10 -> AGC Noise Gate RMS. 5 = the firmware default
 * of 0.0006, 1 = 0.0001 (ignore almost nothing), 10 = 0.0040 (ignore a lot).
 *
 * This mapping runs the OPPOSITE way to the two above — a higher setting means a higher
 * threshold — which is exactly the case the shared inverse above had to be rewritten to handle.
 *
 * The margin between quiet-room p95 (0.00049) and normal-volume music p5 (0.00061) is only
 * 1.25x, so the useful band is narrow: steps 4-6 are where nearly all real tuning happens.
 */
const GATE_CURVE: MacroCurve = { key: "agcNoiseGateRms", lowFactor: 1 / 6, highFactor: 20 / 3 };

const MACRO_CURVES: Partial<Record<AudioParamKey, MacroCurve>> = {
    beatAlpha: ALPHA_CURVE,
    beatSfDelta: DELTA_CURVE,
    agcNoiseGateRms: GATE_CURVE,
};

/**
 * Where `value` sits on the shared 1..10 sensitivity scale, WITHOUT rounding to a step.
 *
 * `sensitivityFromAlpha` and friends answer "which macro step is this exactly", and correctly
 * return null for anything between steps. This answers the different question "how sensitive is
 * this, on the common scale" — which is what translating between two macros needs, since the
 * alpha and delta curves are independent calibrations of the same perceptual axis. Used to give a
 * preset one sensitivity intent that both threshold modes can carry.
 */
export function macroScaleOf(key: AudioParamKey, value: number): number | null {
    const curve = MACRO_CURVES[key];
    if (!curve) return null;

    const mid = macroMid(curve);
    for (const candidate of twoSidedGeometricCandidates(value, mid, curve.lowFactor, curve.highFactor)) {
        if (candidate < SENSITIVITY_MIN || candidate > SENSITIVITY_MAX) continue;
        const reconstructed = twoSidedGeometric(candidate, mid, curve.lowFactor, curve.highFactor);
        if (Math.abs(reconstructed - value) <= 1e-9 * Math.max(Math.abs(value), 1e-12)) {
            return candidate;
        }
    }
    return null;
}

/**
 * The macro step whose value sits closest to `value`, for RENDERING a device that is off the
 * 1..10 grid — never for writing.
 *
 * A board tuned over the serial shell, or carrying a pre-retune persisted value, has no exact
 * macro step: the inverse correctly returns null, which is how "Custom" is detected. But a
 * Simple-mode slider still has to put its thumb somewhere, and pinning it at 0 while the caption
 * says "move the slider to take control" is worse than approximate.
 *
 * Compared in LOG space, because every macro curve is geometric: linear distance would call 0.31
 * nearer to 0.30 than 0.29 is, which is not what "nearest step" means on a ratio scale.
 */
export function nearestMacroStep(key: AudioParamKey, value: number): number | null {
    const curve = MACRO_CURVES[key];
    if (!curve || !(value > 0)) return null;

    let best = SENSITIVITY_MIN;
    let bestDistance = Infinity;
    for (let s = SENSITIVITY_MIN; s <= SENSITIVITY_MAX; s++) {
        const candidate = macroForward(curve, s);
        if (!(candidate > 0)) continue;
        const distance = Math.abs(Math.log(candidate / value));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = s;
        }
    }
    return best;
}

export function gateFromNoiseLevel(s: number | "off"): number {
    if (s === "off") return 0;
    return macroForward(GATE_CURVE, s);
}
export function noiseLevelFromGate(gate: number): number | "off" | null {
    if (gate === 0) return "off";
    return macroInverse(GATE_CURVE, gate);
}

export interface BeatFeelPreset {
    label: string;
    refractoryFrames: number;
    blurb: string;
}

export const BEAT_FEEL_PRESETS: BeatFeelPreset[] = [
    { label: "Punchy", refractoryFrames: 3, blurb: "catches fast drum fills" },
    { label: "Normal", refractoryFrames: 5, blurb: "the default" },
    { label: "Kick only", refractoryFrames: 12, blurb: "locks onto the bass drum, ignores snares and hats" },
];

export function beatFeelFromFrames(frames: number): string | null {
    return BEAT_FEEL_PRESETS.find(p => p.refractoryFrames === frames)?.label ?? null;
}

export interface AdaptSpeedPreset {
    label: string;
    attackFrames: number;
    releaseFrames: number;
    rateLimitFrames: number;
    blurb: string;
}

export const ADAPT_SPEED_PRESETS: AdaptSpeedPreset[] = [
    { label: "Slow", attackFrames: 6, releaseFrames: 30, rateLimitFrames: 20, blurb: "steady; will not chase" },
    { label: "Normal", attackFrames: 3, releaseFrames: 15, rateLimitFrames: 10, blurb: "the default" },
    { label: "Fast", attackFrames: 2, releaseFrames: 6, rateLimitFrames: 4, blurb: "keeps up with big volume swings, can pump" },
];

export function adaptSpeedFromFrames(
    attackFrames: number,
    releaseFrames: number,
    rateLimitFrames: number,
): string | null {
    return (
        ADAPT_SPEED_PRESETS.find(
            p =>
                p.attackFrames === attackFrames &&
                p.releaseFrames === releaseFrames &&
                p.rateLimitFrames === rateLimitFrames,
        )?.label ?? null
    );
}

/* ------------------------------------------------------------------------------------------
 * Synthetic specs for the Simple-mode macro sliders.
 *
 * These are not characteristics — they are 1..10 abstractions over one or more real parameters.
 * Giving them the same shape as a real spec lets the same slider component render both, so
 * Simple and Advanced mode cannot drift apart visually.
 * ---------------------------------------------------------------------------------------- */

/** Sensitivity 1..10. Writes Beat Alpha, or Beat SF Delta when the device is in median mode. */
export const SENSITIVITY_MACRO_SPEC: AudioParamSpec = {
    key: "beatAlpha",
    uuid: "macro:sensitivity",
    cpfFormat: BLE_GATT_CPF_FORMAT_FLOAT32,
    kind: "uint",
    firmwareLabel: "Beat Alpha",
    friendlyLabel: "Sensitivity",
    group: "beat",
    min: SENSITIVITY_MIN,
    max: SENSITIVITY_MAX,
    defaultValue: SENSITIVITY_DEFAULT,
    scale: "linear",
    step: 1,
    displayUnit: "raw",
    decimals: 0,
    help: "Lower = only the biggest hits. Higher = more of the music.",
    detail:
        "The detector keeps a one-second memory of how much the sound has been changing, and " +
        "fires when the current moment stands out from it. This slider moves how far above that " +
        "memory a sound has to be. 5 is what the glasses ship with.",
    advancedOnly: false,
};

/** "Ignore background noise": 0 = Off, then 1..10. Writes the AGC noise gate. */
export const NOISE_MACRO_SPEC: AudioParamSpec = {
    key: "agcNoiseGateRms",
    uuid: "macro:noise",
    cpfFormat: BLE_GATT_CPF_FORMAT_FLOAT32,
    kind: "uint",
    firmwareLabel: "AGC Noise Gate RMS",
    friendlyLabel: "Ignore background noise",
    group: "agc",
    min: 0,
    max: SENSITIVITY_MAX,
    defaultValue: SENSITIVITY_DEFAULT,
    scale: "linear",
    step: 1,
    allowsZero: true,
    zeroLabel: "Off - never mute",
    displayUnit: "raw",
    decimals: 0,
    help: "If the lights do nothing until you turn the music up, turn this down.",
    detail:
        "Anything quieter than this counts as silence: the lights stop reacting entirely and the " +
        "mic level is held where it is. It is the most important control in a difficult room. Too " +
        "high and quiet music is ignored completely - the giveaway is that turning the music up " +
        "appears to fix it. Too low and the air conditioning becomes a drum beat.",
    advancedOnly: false,
};
