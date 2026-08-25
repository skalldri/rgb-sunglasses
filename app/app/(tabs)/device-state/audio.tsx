import { useRouter } from "expo-router";
import React, { useCallback, useMemo, useState } from "react";
import { Pressable, ScrollView, StyleSheet, View } from "react-native";
import { SafeAreaView } from "react-native-safe-area-context";

import { AudioHelpSheet, type AudioHelpContent } from "@/components/audio/audio-help-sheet";
import { ParamChoiceRow } from "@/components/audio/param-choice-row";
import { ParamSliderRow } from "@/components/audio/param-slider-row";
import { ThemedText } from "@/components/themed-text";
import { Card } from "@/components/ui/card";
import { Divider } from "@/components/ui/divider";
import { EmptyState } from "@/components/ui/empty-state";
import { IconSymbol } from "@/components/ui/icon-symbol";
import { SegmentedControl } from "@/components/ui/segmented-control";
import { UUID_AUDIO_CONFIG_SERVICE } from "@/constants/bluetooth";
import { Spacing } from "@/constants/theme";
import { useBluetooth } from "@/context/bluetooth-context";
import { useAudioParamWriter } from "@/hooks/use-audio-param-writer";
import { useDisconnectRedirect } from "@/hooks/use-disconnect-redirect";
import { useThemeColors } from "@/hooks/use-theme-color";
import {
    ADAPT_SPEED_PRESETS,
    AUDIO_PARAMS,
    AUDIO_PARAM_ORDER,
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
export default function AudioTuningScreen() {
    const router = useRouter();
    const c = useThemeColors();
    const { selectedDevice, writeServiceCharacteristic } = useBluetooth();
    useDisconnectRedirect();

    const [mode, setMode] = useState<Mode>("simple");
    const [help, setHelp] = useState<AudioHelpContent | null>(null);

    const serviceChars = selectedDevice?.characteristicsByService?.[UUID_AUDIO_CONFIG_SERVICE];

    const write = useCallback(
        (uuid: string, encoded: string) =>
            writeServiceCharacteristic(UUID_AUDIO_CONFIG_SERVICE, uuid, encoded),
        [writeServiceCharacteristic],
    );
    const writer = useAudioParamWriter(useMemo(() => ({ write }), [write]));

    const resolved = useMemo(() => resolveAudioParams(serviceChars ?? {}), [serviceChars]);
    const byKey = useMemo(() => {
        const map = {} as Partial<Record<AudioParamKey, (typeof resolved)[number]>>;
        resolved.forEach(r => {
            map[r.spec.key] = r;
        });
        return map;
    }, [resolved]);

    /** Current value for a parameter, preferring a local override while the thumb is owned. */
    const valueOf = useCallback(
        (key: AudioParamKey): number | null => {
            const entry = byKey[key];
            if (!entry) return null;
            return writer.displayValue(entry.spec.uuid, entry.value);
        },
        [byKey, writer],
    );

    const busyOf = useCallback(
        (key: AudioParamKey): boolean => serviceChars?.[AUDIO_PARAMS[key].uuid]?.isUpdateInProgress ?? false,
        [serviceChars],
    );

    const writeParam = useCallback(
        (key: AudioParamKey, value: number) => {
            const spec = AUDIO_PARAMS[key];
            return writer.writeNow(spec.uuid, value, v => encodeParam(spec, v));
        },
        [writer],
    );

    const showHelpFor = useCallback((key: AudioParamKey) => {
        const spec = AUDIO_PARAMS[key];
        setHelp({ title: spec.friendlyLabel, firmwareLabel: spec.firmwareLabel, body: spec.detail });
    }, []);

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

    const onSensitivitySlide = useCallback(
        (step: number) => {
            const spec = AUDIO_PARAMS[sensitivityKey];
            writer.onSlide(spec.uuid, sensitivityToParam(step), v => encodeParam(spec, v));
        },
        [sensitivityKey, sensitivityToParam, writer],
    );

    const onSensitivityComplete = useCallback(
        (step: number) => {
            const spec = AUDIO_PARAMS[sensitivityKey];
            writer.onSlideComplete(spec.uuid, sensitivityToParam(step), v => encodeParam(spec, v));
        },
        [sensitivityKey, sensitivityToParam, writer],
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
            // Sequential: Android permits one outstanding GATT operation, and firing three
            // writes concurrently gets two of them rejected rather than queued.
            await writeParam("agcAttackFrames", preset.attackFrames);
            await writeParam("agcReleaseFrames", preset.releaseFrames);
            await writeParam("agcRateLimitFrames", preset.rateLimitFrames);
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

    if (!serviceChars || resolved.length === 0) {
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
                                firmwareLabel: AUDIO_PARAMS[sensitivityKey].firmwareLabel,
                            }}
                            value={sensitivity}
                            busy={busyOf(sensitivityKey)}
                            liveNote={
                                sensitivity === null && sensitivityRaw !== null
                                    ? `Custom (${formatParamValue(AUDIO_PARAMS[sensitivityKey], sensitivityRaw)}) - move the slider to take control`
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
                            options={BEAT_FEEL_PRESETS.map(p => ({ label: p.label, blurb: p.blurb }))}
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
                            options={ADAPT_SPEED_PRESETS.map(p => ({ label: p.label, blurb: p.blurb }))}
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
                        busyOf={busyOf}
                        onWrite={writeParam}
                        onHelp={showHelpFor}
                        writer={writer}
                    />
                )}
            </ScrollView>

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
    busyOf,
    onWrite,
    onHelp,
    writer,
}: {
    resolved: ReturnType<typeof resolveAudioParams>;
    valueOf: (key: AudioParamKey) => number | null;
    busyOf: (key: AudioParamKey) => boolean;
    onWrite: (key: AudioParamKey, value: number) => Promise<boolean>;
    onHelp: (key: AudioParamKey) => void;
    writer: ReturnType<typeof useAudioParamWriter>;
}) {
    const c = useThemeColors();
    const usesMedian = Math.round(valueOf("beatThresholdMode") ?? 0) === THRESHOLD_MODE_MEDIAN;

    const groups: ("agc" | "beat" | "display")[] = ["agc", "beat", "display"];

    return (
        <>
            {groups.map(group => {
                const keys = AUDIO_PARAM_ORDER.filter(key => {
                    if (AUDIO_PARAMS[key].group !== group) return false;
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
                            const spec = AUDIO_PARAMS[key];
                            const value = valueOf(key);

                            if (spec.kind === "enum") {
                                const labels = spec.enumLabels ?? [];
                                return (
                                    <React.Fragment key={key}>
                                        {index > 0 ? <Divider /> : null}
                                        <ParamChoiceRow
                                            title={spec.friendlyLabel}
                                            firmwareLabel={spec.firmwareLabel}
                                            testID={`choice-${spec.key}`}
                                            options={labels.map(label => ({ label }))}
                                            selected={value === null ? null : labels[Math.round(value)] ?? null}
                                            help={spec.help}
                                            busy={busyOf(key)}
                                            onSelect={label => {
                                                const next = labels.indexOf(label);
                                                if (next >= 0) void onWrite(key, next);
                                            }}
                                            onHelp={() => onHelp(key)}
                                        />
                                    </React.Fragment>
                                );
                            }

                            return (
                                <React.Fragment key={key}>
                                    {index > 0 ? <Divider /> : null}
                                    <ParamSliderRow
                                        spec={spec}
                                        value={value}
                                        busy={busyOf(key)}
                                        showFirmwareLabel
                                        onSlide={v => writer.onSlide(spec.uuid, v, x => encodeParam(spec, x))}
                                        onSlideComplete={v =>
                                            writer.onSlideComplete(spec.uuid, v, x => encodeParam(spec, x))
                                        }
                                        onHelp={() => onHelp(key)}
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

const styles = StyleSheet.create({
    screen: { flex: 1 },
    header: { flexDirection: "row", alignItems: "center", paddingHorizontal: Spacing.lg, height: 44 },
    back: { flexDirection: "row", alignItems: "center", gap: Spacing.xs },
    content: { padding: Spacing.lg, gap: Spacing.md, paddingBottom: Spacing.xxl },
    groupHeader: { gap: 2, paddingBottom: Spacing.xs },
});
