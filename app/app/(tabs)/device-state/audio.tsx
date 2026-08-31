import { useRouter } from "expo-router";
import React, { memo, useCallback, useEffect, useMemo, useRef, useState } from "react";
import { Pressable, ScrollView, StyleSheet, View } from "react-native";
import { SafeAreaView } from "react-native-safe-area-context";

import { AudioHelpSheet, type AudioHelpContent } from "@/components/audio/audio-help-sheet";
import { MonitorPanel } from "@/components/audio/monitor-panel";
import { useAudioTelemetryStream } from "@/context/audio-telemetry-context";
import { PresetSheet } from "@/components/audio/preset-sheet";
import { ParamChoiceRow } from "@/components/audio/param-choice-row";
import { ParamSliderRow } from "@/components/audio/param-slider-row";
import { ThemedText } from "@/components/themed-text";
import { AppButton } from "@/components/ui/app-button";
import { Card } from "@/components/ui/card";
import { Divider } from "@/components/ui/divider";
import { EmptyState } from "@/components/ui/empty-state";
import { IconSymbol } from "@/components/ui/icon-symbol";
import { SegmentedControl } from "@/components/ui/segmented-control";
import { Radii, Spacing } from "@/constants/theme";
import { useBluetooth } from "@/context/bluetooth-context";
import { type AudioParamWriter } from "@/hooks/use-audio-param-writer";
import { useAudioParams } from "@/hooks/use-audio-params";
import { useAudioPresets } from "@/hooks/use-audio-presets";
import { useDisconnectRedirect } from "@/hooks/use-disconnect-redirect";
import { useThemeColors } from "@/hooks/use-theme-color";
import { AudioPreset, diffPreset, suggestPresetName } from "@/services/audio-presets";
import {
    ADAPT_SPEED_PRESETS,
    AUDIO_PARAMS,
    AUDIO_PARAM_ORDER,
    type AudioParamSpec,
    AudioParamKey,
    BEAT_FEEL_PRESETS,
    NOISE_MACRO_SPEC,
    SENSITIVITY_MACRO_SPEC,
    adaptSpeedFromFrames,
    alphaFromSensitivity,
    beatFeelFromFrames,
    deltaFromSensitivity,
    encodeParam,
    formatParamValue,
    gateFromNoiseLevel,
    nearestMacroStep,
    noiseLevelFromGate,
    paramFramesToMs,
    resolveAudioParams,
    sensitivityFromAlpha,
    sensitivityFromDelta,
} from "@/services/audio-params";

type Mode = "simple" | "advanced";

const THRESHOLD_MODE_MEDIAN = 1;

/**
 * Audio Tuning.
 *
 * Replaces the generic 14-text-box rendering of the Audio Analysis Config service. The premise
 * is that AGC and beat detection have to be tuned at the venue, by someone standing in a loud
 * dark room holding a phone one-handed — so the default view is four plain-language controls,
 * and the 14 real parameters live behind an Advanced tab that still shows each firmware name so
 * it greps against `sound dsp params` and fw/docs/beat-detection-debugging.md.
 *
 * Values come from discovery (which already reads every characteristic) and from the clamp
 * read-backs that `bluetooth-context.tsx` schedules after each write. This screen deliberately
 * does NOT re-read on focus: these characteristics are not notifiable, and a read-on-focus
 * effect that writes results into context is the exact shape of the unbounded-read-loop bug
 * documented in app/CLAUDE.md. If a stale value ever proves to be a real problem, the fix is a
 * ref-driven one-shot read, not a reactive effect.
 */
/* No provider here either — the stack layout owns the single AudioTelemetryProvider. */
export default function AudioTuningScreen() {

    const router = useRouter();
    const c = useThemeColors();
    const { selectedDevice } = useBluetooth();
    useDisconnectRedirect();

    const [mode, setMode] = useState<Mode>("simple");
    const [help, setHelp] = useState<AudioHelpContent | null>(null);
    const [presetsOpen, setPresetsOpen] = useState(false);
    const [toast, setToast] = useState<string | null>(null);

    /* All parameter plumbing comes from ONE hook, shared with the calibration wizard. It used
     * to be ~60 lines copied between the two screens, and the copies had already diverged on
     * how they filtered a missing value (`typeof v === "number"` here vs `v !== null` there). */
    // This screen renders meters, so it is what keeps the telemetry stream armed.
    useAudioTelemetryStream();

    const {
        resolved,
        currentValues,
        valueOf,
        specOf,
        busyOf,
        writeParam,
        writer,
        absent,
        writeFailure,
    } = useAudioParams();

    // The named writeParam above, not a second copy of it: an inline duplicate was a second
    // identity that would silently diverge the moment one of them grew (a busy guard, a retry).
    const presets = useAudioPresets({ currentValues, writeParam });

    /* Only the sheet consumes this, and only while it is open.
     *
     * The dependency was the whole `presets` object, which was a fresh literal every render, so
     * this useMemo never hit — it re-diffed every preset against all 14 parameters on every
     * render, including every frame of every slider drag, with the sheet closed. The hook now
     * returns a stable object, and the gate means a drag does no preset work at all.
     *
     * `diffPreset(currentValues, ...)` rather than the hook's `previewDiff`, which reads the
     * values out of a ref: a memo keyed on `currentValues` would then be claiming a dependency
     * that could not actually invalidate it. Same result, deps that mean what they say. */
    const allPresets = presets.allPresets;
    const changeCounts = useMemo(() => {
        if (!presetsOpen) return EMPTY_CHANGE_COUNTS;
        const out: Record<string, number> = {};
        allPresets.forEach(p => {
            out[p.id] = diffPreset(currentValues, p).length;
        });
        return out;
    }, [presetsOpen, allPresets, currentValues]);

    /* Transient status line. Deliberately not a modal: at a venue the user is looking at the
     * glasses, not the phone, and a dialog would demand a dismissing tap they cannot spare.
     *
     * The timer is tracked and cleared, because a bare setTimeout here outlives the screen — it
     * would fire setToast into an unmounted component (and hold the jest worker open, which is
     * how this was caught). */
    const toastTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
    const announce = useCallback((message: string) => {
        if (toastTimer.current) clearTimeout(toastTimer.current);
        setToast(message);
        toastTimer.current = setTimeout(() => {
            toastTimer.current = null;
            setToast(null);
        }, 4000);
    }, []);

    useEffect(
        () => () => {
            if (toastTimer.current) clearTimeout(toastTimer.current);
        },
        [],
    );

    const handleApplyPreset = useCallback(
        async (preset: AudioPreset) => {
            setPresetsOpen(false);
            const { applied, failed } = await presets.applyPreset(preset);
            announce(
                failed.length > 0
                    ? `${preset.name}: ${applied} applied, ${failed.length} failed`
                    : `Applied "${preset.name}" (${applied} changed)`,
            );
        },
        [announce, presets],
    );

    const handleSwap = useCallback(async () => {
        const outcome = await presets.swapAB();
        if (outcome.kind === "unassigned") {
            announce("Assign a preset to both A and B first");
            return;
        }
        if (outcome.kind === "identical") {
            // Reachable, and it used to report "Swapped (0 changed)" — success for an action that
            // did nothing and could not do anything until a slot is reassigned.
            announce("A and B match here - nothing to change");
            return;
        }
        const { preset, result } = outcome;
        announce(
            result.failed.length > 0
                ? `${preset.name}: ${result.applied} applied, ${result.failed.length} failed`
                : `Now on "${preset.name}" (${result.applied} changed)`,
        );
    }, [announce, presets]);

    const handleUndo = useCallback(async () => {
        const top = presets.undoStack[0];
        if (!top) {
            announce("Nothing to undo");
            return;
        }
        const { applied, failed } = await presets.undo();
        // `applied` alone read every outcome as success, so a restore in which every write failed
        // announced "(0 restored)" in the same confident phrasing as one that worked.
        announce(
            failed.length > 0
                ? `Could not undo ${top.label}: ${failed.length} of ${applied + failed.length} failed`
                : `Undid: ${top.label} (${applied} restored)`,
        );
    }, [announce, presets]);

    const handleSavePreset = useCallback(
        (name: string) => {
            const saved = presets.saveCurrentAs(name, Date.now());
            setPresetsOpen(false);
            if (!saved) {
                announce("Nothing to save yet");
                return;
            }
            announce(
                !saved.persisted
                    ? `Could not save "${name}" to storage - it will be gone next launch`
                    : saved.replaced
                      ? `Replaced "${name}"`
                      : `Saved "${name}"`,
            );
        },
        [announce, presets],
    );

    const handleDeletePreset = useCallback(
        (id: string) => {
            const name = presets.findPreset(id)?.name ?? "preset";
            if (!presets.deletePreset(id)) {
                announce(`Could not remove "${name}" from storage - it will be back next launch`);
            }
        },
        [announce, presets],
    );

    const showHelpFor = useCallback(
        (key: AudioParamKey) => {
            const spec = specOf(key);
            setHelp({ title: spec.friendlyLabel, firmwareLabel: spec.firmwareLabel, body: spec.detail });
        },
        [specOf],
    );

    const closeHelp = useCallback(() => setHelp(null), []);

    /* -------------------------------------------------------------------------------------
     * Simple-mode derived state
     * ----------------------------------------------------------------------------------- */

    const thresholdMode = valueOf("beatThresholdMode") ?? 0;
    const usesMedian = Math.round(thresholdMode) === THRESHOLD_MODE_MEDIAN;

    // Which real parameter "Sensitivity" drives depends on the threshold shape. Showing a
    // slider that silently writes the parameter the device is NOT using would be worse than
    // showing nothing, so the label follows the mode too.
    const sensitivityKey: AudioParamKey = usesMedian ? "beatSfDelta" : "beatAlpha";
    const sensitivityRaw = valueOf(sensitivityKey);
    const sensitivity =
        sensitivityRaw === null
            ? null
            : usesMedian
              ? sensitivityFromDelta(sensitivityRaw)
              : sensitivityFromAlpha(sensitivityRaw);

    const gateRaw = valueOf("agcNoiseGateRms");
    const noiseLevel = gateRaw === null ? null : noiseLevelFromGate(gateRaw);

    const refractory = valueOf("beatRefractoryFrames");
    const beatFeel = refractory === null ? null : beatFeelFromFrames(Math.round(refractory));

    const attack = valueOf("agcAttackFrames");
    const release = valueOf("agcReleaseFrames");
    const rateLimit = valueOf("agcRateLimitFrames");
    const adaptSpeed =
        attack === null || release === null || rateLimit === null
            ? null
            : adaptSpeedFromFrames(Math.round(attack), Math.round(release), Math.round(rateLimit));

    /**
     * The macro sliders go through the SAME throttle as the raw ones.
     *
     * What is stored as the local override is the underlying parameter value (an alpha, a gate),
     * not the 1..10 step — because the thumb position is derived by inverting the mapping from
     * whatever value the parameter currently holds. Storing the step here would make the
     * inversion fail and the control would render "Custom" while the user was dragging it.
     */
    const sensitivityToParam = useCallback(
        (step: number) => (usesMedian ? deltaFromSensitivity(step) : alphaFromSensitivity(step)),
        [usesMedian],
    );

    /* Every encode path goes through the RESOLVED spec, so encodeParam clamps against the
     * firmware's published range rather than the app's mirror of it. Reaching into
     * AUDIO_PARAMS here is what made the whole ranges feature unobservable. */
    const onSensitivitySlide = useCallback(
        (step: number) => {
            const spec = specOf(sensitivityKey);
            writer.onSlide(spec.uuid, sensitivityToParam(step), v => encodeParam(spec, v));
        },
        [sensitivityKey, sensitivityToParam, writer, specOf],
    );

    const onSensitivityComplete = useCallback(
        (step: number) => {
            const spec = specOf(sensitivityKey);
            writer.onSlideComplete(spec.uuid, sensitivityToParam(step), v => encodeParam(spec, v));
        },
        [sensitivityKey, sensitivityToParam, writer, specOf],
    );

    const noiseToParam = useCallback(
        (step: number) => gateFromNoiseLevel(step === 0 ? "off" : step),
        [],
    );

    const onNoiseSlide = useCallback(
        (step: number) => {
            const spec = AUDIO_PARAMS.agcNoiseGateRms;
            writer.onSlide(spec.uuid, noiseToParam(step), v => encodeParam(spec, v));
        },
        [noiseToParam, writer],
    );

    const onNoiseComplete = useCallback(
        (step: number) => {
            const spec = AUDIO_PARAMS.agcNoiseGateRms;
            writer.onSlideComplete(spec.uuid, noiseToParam(step), v => encodeParam(spec, v));
        },
        [noiseToParam, writer],
    );

    const onBeatFeel = useCallback(
        (label: string) => {
            const preset = BEAT_FEEL_PRESETS.find(p => p.label === label);
            if (preset) void writeParam("beatRefractoryFrames", preset.refractoryFrames);
        },
        [writeParam],
    );

    const onAdaptSpeed = useCallback(
        async (label: string) => {
            const preset = ADAPT_SPEED_PRESETS.find(p => p.label === label);
            if (!preset) return;
            /* Sequential: Android permits one outstanding GATT operation, and firing three writes
             * concurrently gets two of them rejected rather than queued.
             *
             * STOP AT THE FIRST FAILURE. writeParam resolves false rather than throwing, so
             * discarding these results let a mid-sequence failure carry on and leave a torn
             * preset — attack at Fast's 2 frames while release and rate limit kept the previous
             * preset's values, a combination no preset defines. adaptSpeedFromFrames then maps
             * that to null and the row just reads "Custom", with nothing saying a write failed.
             * The device is still torn either way, but stopping keeps it one write from the old
             * preset rather than two, and the banner below now says so. */
            const steps: [AudioParamKey, number][] = [
                ["agcAttackFrames", preset.attackFrames],
                ["agcReleaseFrames", preset.releaseFrames],
                ["agcRateLimitFrames", preset.rateLimitFrames],
            ];
            for (const [key, value] of steps) {
                if (!(await writeParam(key, value))) return;
            }
        },
        [writeParam],
    );

    /* -------------------------------------------------------------------------------------
     * Render
     * ----------------------------------------------------------------------------------- */

    if (!selectedDevice) {
        return (
            <SafeAreaView style={[styles.screen, { backgroundColor: c.background }]} edges={["top"]}>
                <EmptyState title="Not connected" subtitle="Connect to your glasses to tune audio." />
            </SafeAreaView>
        );
    }

    if (absent) {
        return (
            <SafeAreaView style={[styles.screen, { backgroundColor: c.background }]} edges={["top"]}>
                <Header onBack={() => router.back()} />
                <EmptyState
                    title="No audio tuning on this firmware"
                    subtitle="This device does not expose the audio analysis service. Update the firmware to tune AGC and beat detection."
                />
            </SafeAreaView>
        );
    }

    return (
        <SafeAreaView style={[styles.screen, { backgroundColor: c.background }]} edges={["top"]}>
            <Header onBack={() => router.back()} />

            <ScrollView contentContainerStyle={styles.content}>
                <ThemedText type="heading">Audio Tuning</ThemedText>
                <ThemedText type="caption" style={{ color: c.textSecondary }}>
                    How the glasses listen to the room.
                </ThemedText>

                {/* This screen used to surface write failures NOWHERE — alone among the
                    device-state screens. A slider write that failed showed the thumb for the
                    settle window and then silently snapped back, which at a venue is
                    indistinguishable from the firmware clamping the value.

                    Read from the context's own lastWriteError rather than the writer's onError
                    callback: onError only fires when the write function THROWS, and the writer
                    this screen is wired to (writeServiceCharacteristic) never throws — it
                    catches every BLE error and returns false. The context records the reason on
                    the characteristic either way, so that is the surface that actually sees a
                    real failure. */}
                {writeFailure ? (
                    <ThemedText
                        type="caption"
                        style={{ color: c.danger }}
                        testID="audio-write-error"
                    >
                        Could not set {writeFailure.label}: {writeFailure.reason}
                    </ThemedText>
                ) : null}

                {/* The AGC window and the gate are drawn ON the level meter, so the meter needs
                    the live values of those three parameters. They come from the same resolved
                    table the sliders edit, which is what keeps the drawn window and the dragged
                    slider showing the same number. */}
                <MonitorPanel
                    targetLow={valueOf("agcTargetLow")}
                    targetHigh={valueOf("agcTargetHigh")}
                    noiseGate={valueOf("agcNoiseGateRms")}
                    // The banner has to name a control the user can actually see. In Advanced
                    // there is no 1..10 "Sensitivity" — there are the raw thresholds, which run
                    // the OTHER WAY, so unqualified "turn Sensitivity down" advice read there
                    // sends someone the wrong direction. Same sensitivityKey the Simple macro
                    // uses, so the advice always tracks the threshold shape in effect.
                    sensitivity={{ mode, key: sensitivityKey as "beatAlpha" | "beatSfDelta" }}
                />

                {/* The wizard is a pushed route, not a modal: it holds attention for ~30 s in a
                    loud room and needs an unambiguous Cancel at every point. */}
                <Pressable
                    testID="audio-open-wizard"
                    onPress={() => router.push("/(tabs)/device-state/audio-calibrate")}
                    accessibilityRole="button"
                    style={[styles.wizardButton, { backgroundColor: c.surface, borderColor: c.border }]}
                >
                    <View style={{ flex: 1 }}>
                        <ThemedText type="defaultSemiBold">✨  Tune it for me</ThemedText>
                        <ThemedText type="caption" style={{ color: c.textSecondary }}>
                            Measures the room, then fits everything to it. About a minute.
                        </ThemedText>
                    </View>
                    <IconSymbol name="chevron.right" size={16} color={c.textMuted} />
                </Pressable>

                <SegmentedControl<Mode>
                    options={[
                        { label: "Simple", value: "simple" },
                        { label: "Advanced", value: "advanced" },
                    ]}
                    value={mode}
                    onChange={setMode}
                />

                {mode === "simple" ? (
                    <Card>
                        <ParamSliderRow
                            spec={{
                                ...SENSITIVITY_MACRO_SPEC,
                                // Identity follows the mode, not just the label: `key` drives the
                                // testID and accessibility id, and a control that still called
                                // itself "beatAlpha" while writing SF Delta would be untappable by
                                // name and confusing to debug.
                                key: sensitivityKey,
                                firmwareLabel: specOf(sensitivityKey).firmwareLabel,
                            }}
                            value={sensitivity}
                            /* Off-grid device: put the thumb on the nearest step and keep the
                             * slider live, so the "move the slider to take control" caption
                             * below is actually followable. */
                            ghostValue={
                                sensitivity === null && sensitivityRaw !== null
                                    ? nearestMacroStep(sensitivityKey, sensitivityRaw)
                                    : null
                            }
                            busy={busyOf(sensitivityKey)}
                            liveNote={
                                sensitivity === null && sensitivityRaw !== null
                                    ? `Custom (${formatParamValue(specOf(sensitivityKey), sensitivityRaw)}) - move the slider to take control`
                                    : null
                            }
                            onSlide={onSensitivitySlide}
                            onSlideComplete={onSensitivityComplete}
                            onHelp={() => setHelp({
                                title: SENSITIVITY_MACRO_SPEC.friendlyLabel,
                                firmwareLabel: usesMedian ? "Beat SF Delta" : "Beat Alpha",
                                body: SENSITIVITY_MACRO_SPEC.detail,
                            })}
                        />
                        <Divider />

                        <ParamChoiceRow
                            title="Beat feel"
                            testID="choice-beat-feel"
                            options={BEAT_FEEL_OPTIONS}
                            selected={beatFeel}
                            customLabel={refractory === null ? null : `${paramFramesToMs(refractory)} ms`}
                            help="How long to ignore a band after it fires."
                            busy={busyOf("beatRefractoryFrames")}
                            onSelect={onBeatFeel}
                            onHelp={() => showHelpFor("beatRefractoryFrames")}
                        />
                        <Divider />

                        <ParamSliderRow
                            spec={NOISE_MACRO_SPEC}
                            value={noiseLevel === "off" ? 0 : noiseLevel}
                            ghostValue={
                                noiseLevel === null && gateRaw !== null
                                    ? nearestMacroStep("agcNoiseGateRms", gateRaw)
                                    : null
                            }
                            busy={busyOf("agcNoiseGateRms")}
                            liveNote={
                                noiseLevel === null && gateRaw !== null
                                    ? `Custom (${formatParamValue(AUDIO_PARAMS.agcNoiseGateRms, gateRaw)}) - move the slider to take control`
                                    : null
                            }
                            onSlide={onNoiseSlide}
                            onSlideComplete={onNoiseComplete}
                            onHelp={() => setHelp({
                                title: NOISE_MACRO_SPEC.friendlyLabel,
                                firmwareLabel: NOISE_MACRO_SPEC.firmwareLabel,
                                body: NOISE_MACRO_SPEC.detail,
                            })}
                        />
                        <Divider />

                        <ParamChoiceRow
                            title="How fast it adapts"
                            testID="choice-adapt-speed"
                            options={ADAPT_SPEED_OPTIONS}
                            selected={adaptSpeed}
                            customLabel={
                                attack === null || release === null
                                    ? null
                                    : `${paramFramesToMs(attack)} / ${paramFramesToMs(release)} ms`
                            }
                            help="How quickly the glasses adjust as the room gets louder or quieter."
                            busy={busyOf("agcAttackFrames") || busyOf("agcReleaseFrames") || busyOf("agcRateLimitFrames")}
                            onSelect={onAdaptSpeed}
                            onHelp={() => showHelpFor("agcAttackFrames")}
                        />
                    </Card>
                ) : (
                    <AdvancedGroups
                        resolved={resolved}
                        valueOf={valueOf}
                        specOf={specOf}
                        busyOf={busyOf}
                        onWrite={writeParam}
                        onHelp={showHelpFor}
                        writer={writer}
                    />
                )}
            </ScrollView>

            {toast ? (
                <View style={[styles.toast, { backgroundColor: c.surfaceAlt, borderColor: c.border }]}>
                    <ThemedText type="caption" testID="audio-toast" style={{ color: c.textPrimary }}>
                        {toast}
                    </ThemedText>
                </View>
            ) : null}

            {/* Sticky footer: the three things you reach for mid-set, always in thumb range. */}
            <View style={[styles.footer, { borderTopColor: c.border, backgroundColor: c.background }]}>
                <AppButton
                    title="A ⇄ B"
                    variant="secondary"
                    style={styles.footerButton}
                    testID="audio-swap-ab"
                    disabled={presets.applying || !presets.slotA || !presets.slotB}
                    onPress={handleSwap}
                />
                <AppButton
                    title="Presets"
                    variant="secondary"
                    style={styles.footerButton}
                    testID="audio-open-presets"
                    onPress={() => setPresetsOpen(true)}
                />
                <AppButton
                    title="Undo"
                    variant="secondary"
                    style={styles.footerButton}
                    testID="audio-undo"
                    disabled={!presets.canUndo || presets.applying}
                    onPress={handleUndo}
                />
            </View>

            <PresetSheet
                visible={presetsOpen}
                presets={presets.allPresets}
                slotA={presets.slotA}
                slotB={presets.slotB}
                changeCounts={changeCounts}
                busy={presets.applying}
                suggestedName={suggestPresetName(new Date())}
                onApply={handleApplyPreset}
                onAssignSlot={(slot, id) => (slot === "A" ? presets.setSlotA(id) : presets.setSlotB(id))}
                onDelete={handleDeletePreset}
                onSave={handleSavePreset}
                onClose={() => setPresetsOpen(false)}
            />

            <AudioHelpSheet content={help} onClose={closeHelp} />
        </SafeAreaView>
    );
}

function Header({ onBack }: { onBack: () => void }) {
    const c = useThemeColors();
    return (
        <View style={styles.header}>
            <Pressable
                onPress={onBack}
                hitSlop={12}
                accessibilityRole="button"
                accessibilityLabel="Back to Controls"
                style={styles.back}
            >
                <IconSymbol name="chevron.left" size={22} color={c.textPrimary} />
                <ThemedText style={{ color: c.textPrimary }}>Controls</ThemedText>
            </Pressable>
        </View>
    );
}

const GROUP_TITLES: Record<string, { title: string; subtitle?: string }> = {
    agc: {
        title: "Automatic gain",
        subtitle: "How the glasses set their own mic level as the room changes.",
    },
    beat: { title: "Beat detection", subtitle: "What counts as a beat." },
    display: {
        title: "Bar display only",
        // Without this, non-experts spend ten minutes tuning the visualiser and wondering why
        // beat detection has not changed.
        subtitle: "These only change the bar visualiser. They do not affect beat detection.",
    },
};

function AdvancedGroups({
    resolved,
    valueOf,
    specOf,
    busyOf,
    onWrite,
    onHelp,
    writer,
}: {
    resolved: ReturnType<typeof resolveAudioParams>;
    valueOf: (key: AudioParamKey) => number | null;
    specOf: (key: AudioParamKey) => (typeof AUDIO_PARAMS)[AudioParamKey];
    busyOf: (key: AudioParamKey) => boolean;
    onWrite: (key: AudioParamKey, value: number) => Promise<boolean>;
    onHelp: (key: AudioParamKey) => void;
    writer: ReturnType<typeof useAudioParams>["writer"];
}) {
    const c = useThemeColors();
    const usesMedian = Math.round(valueOf("beatThresholdMode") ?? 0) === THRESHOLD_MODE_MEDIAN;

    const groups: ("agc" | "beat" | "display")[] = ["agc", "beat", "display"];

    return (
        <>
            {groups.map(group => {
                const keys = AUDIO_PARAM_ORDER.filter(key => {
                    if (specOf(key).group !== group) return false;
                    if (!resolved.some(r => r.spec.key === key)) return false;
                    // Only one of alpha/sf_delta is in use at a time. Hiding the inactive one
                    // beats disabling it: a greyed-out slider still invites tuning attempts.
                    if (key === "beatAlpha" && usesMedian) return false;
                    if (key === "beatSfDelta" && !usesMedian) return false;
                    return true;
                });
                if (keys.length === 0) return null;

                const meta = GROUP_TITLES[group];
                return (
                    <Card key={group}>
                        <View style={styles.groupHeader}>
                            <ThemedText type="subtitle">{meta.title}</ThemedText>
                            {meta.subtitle ? (
                                <ThemedText type="caption" style={{ color: c.textSecondary }}>
                                    {meta.subtitle}
                                </ThemedText>
                            ) : null}
                        </View>

                        {keys.map((key, index) => {
                            /* The RESOLVED spec: min/max/step/enum labels come from the
                             * firmware when it publishes them. Rendering from the static
                             * table is what left sliders clamped to stale app-side ranges. */
                            const spec = specOf(key);
                            const value = valueOf(key);

                            if (spec.kind === "enum") {
                                return (
                                    <React.Fragment key={key}>
                                        {index > 0 ? <Divider /> : null}
                                        <AdvancedChoiceRow
                                            spec={spec}
                                            value={value}
                                            busy={busyOf(key)}
                                            onWrite={onWrite}
                                            onHelp={onHelp}
                                        />
                                    </React.Fragment>
                                );
                            }

                            return (
                                <React.Fragment key={key}>
                                    {index > 0 ? <Divider /> : null}
                                    <AdvancedSliderRow
                                        spec={spec}
                                        value={value}
                                        busy={busyOf(key)}
                                        writer={writer}
                                        onHelp={onHelp}
                                    />
                                </React.Fragment>
                            );
                        })}
                    </Card>
                );
            })}
        </>
    );
}

/* Stable per-row wrappers. The Advanced rows used to pass raw inline arrows for onSlide /
 * onSlideComplete / onHelp — and ParamSliderRow's hand-written comparator compares exactly those
 * by identity, so it returned false on every render and the memo it guards never once hit.
 * Building the handlers inside the row, from props that are themselves stable, is what actually
 * delivers the memoization the comparator was written for. */

const NO_LABELS: readonly string[] = [];

const AdvancedSliderRow = memo(function AdvancedSliderRow({
    spec,
    value,
    busy,
    writer,
    onHelp,
}: {
    spec: AudioParamSpec;
    value: number | null;
    busy: boolean;
    writer: AudioParamWriter;
    onHelp: (key: AudioParamKey) => void;
}) {
    const handleSlide = useCallback(
        (v: number) => writer.onSlide(spec.uuid, v, x => encodeParam(spec, x)),
        [writer, spec],
    );
    const handleComplete = useCallback(
        (v: number) => writer.onSlideComplete(spec.uuid, v, x => encodeParam(spec, x)),
        [writer, spec],
    );
    const handleHelp = useCallback(() => onHelp(spec.key), [onHelp, spec.key]);

    return (
        <ParamSliderRow
            spec={spec}
            value={value}
            busy={busy}
            showFirmwareLabel
            onSlide={handleSlide}
            onSlideComplete={handleComplete}
            onHelp={handleHelp}
        />
    );
});

const AdvancedChoiceRow = memo(function AdvancedChoiceRow({
    spec,
    value,
    busy,
    onWrite,
    onHelp,
}: {
    spec: AudioParamSpec;
    value: number | null;
    busy: boolean;
    onWrite: (key: AudioParamKey, value: number) => Promise<boolean> | void;
    onHelp: (key: AudioParamKey) => void;
}) {
    const labels = spec.enumLabels ?? NO_LABELS;
    // Memoised: a fresh array each render defeats ParamChoiceRow's own memo independently of
    // the handlers.
    const options = useMemo(() => labels.map(label => ({ label })), [labels]);
    const handleSelect = useCallback(
        (label: string) => {
            const next = labels.indexOf(label);
            if (next >= 0) void onWrite(spec.key, next);
        },
        [labels, onWrite, spec.key],
    );
    const handleHelp = useCallback(() => onHelp(spec.key), [onHelp, spec.key]);

    return (
        <ParamChoiceRow
            title={spec.friendlyLabel}
            firmwareLabel={spec.firmwareLabel}
            testID={`choice-${spec.key}`}
            options={options}
            selected={value === null ? null : labels[Math.round(value)] ?? null}
            help={spec.help}
            busy={busy}
            onSelect={handleSelect}
            onHelp={handleHelp}
        />
    );
});

/* Hoisted to module scope: these are static, and rebuilding them per render allocated a new
 * array every time, defeating ParamChoiceRow's memo on its own. */
/** Frozen so the closed-sheet path returns a stable identity rather than a fresh literal. */
const EMPTY_CHANGE_COUNTS: Record<string, number> = {};

const BEAT_FEEL_OPTIONS = BEAT_FEEL_PRESETS.map(p => ({ label: p.label, blurb: p.blurb }));
const ADAPT_SPEED_OPTIONS = ADAPT_SPEED_PRESETS.map(p => ({ label: p.label, blurb: p.blurb }));

const styles = StyleSheet.create({
    wizardButton: {
        flexDirection: "row",
        alignItems: "center",
        gap: Spacing.md,
        minHeight: 56,
        paddingHorizontal: Spacing.md,
        paddingVertical: Spacing.sm,
        borderRadius: Radii.md,
        borderWidth: StyleSheet.hairlineWidth,
    },
    screen: { flex: 1 },
    header: { flexDirection: "row", alignItems: "center", paddingHorizontal: Spacing.lg, height: 44 },
    back: { flexDirection: "row", alignItems: "center", gap: Spacing.xs },
    content: { padding: Spacing.lg, gap: Spacing.md, paddingBottom: Spacing.xxl },
    footer: {
        flexDirection: "row",
        gap: Spacing.sm,
        paddingHorizontal: Spacing.lg,
        paddingTop: Spacing.sm,
        paddingBottom: Spacing.sm,
        borderTopWidth: StyleSheet.hairlineWidth,
    },
    // 56 dp: these are the controls reached for one-handed, without looking. Surface, border and
    // disabled dimming come from AppButton's secondary variant — only the size is ours.
    footerButton: { flex: 1, minHeight: 56 },
    toast: {
        position: "absolute",
        left: Spacing.lg,
        right: Spacing.lg,
        bottom: 80,
        padding: Spacing.md,
        borderWidth: 1,
        borderRadius: Radii.md,
    },
    groupHeader: { gap: 2, paddingBottom: Spacing.xs },
});
