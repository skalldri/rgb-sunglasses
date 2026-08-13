import { WriteErrorIndicator } from "@/components/characteristic-write-error";
import { ThemedText } from "@/components/themed-text";
import { AppButton } from "@/components/ui/app-button";
import { Badge } from "@/components/ui/badge";
import { Card } from "@/components/ui/card";
import { Divider } from "@/components/ui/divider";
import { EmptyState } from "@/components/ui/empty-state";
import { IconSymbol } from "@/components/ui/icon-symbol";
import { ListRow } from "@/components/ui/list-row";
import { ProgressBar } from "@/components/ui/progress-bar";
import { Section } from "@/components/ui/section";
import { SegmentedControl } from "@/components/ui/segmented-control";
import {
    UUID_CAPTURE_CONTROL, UUID_CAPTURE_COUNT, UUID_CAPTURE_ELAPSED_S, UUID_CAPTURE_LIMIT_S,
    UUID_CAPTURE_REMAINING_S, UUID_CAPTURE_SERVICE, UUID_CAPTURE_STATE,
} from "@/constants/bluetooth";
import { Spacing } from "@/constants/theme";
import { useBluetooth } from "@/context/bluetooth-context";
import { useDisconnectRedirect } from "@/hooks/use-disconnect-redirect";
import { useScopedCharacteristicMonitors } from "@/hooks/use-scoped-characteristic-monitors";
import { useThemeColors } from "@/hooks/use-theme-color";
import { decodeUint32FromBase64, encodeUint32ToBase64 } from "@/services/ble-value-codec";
import {
    CAPTURE_COMMAND_START, CAPTURE_COMMAND_STOP, CAPTURE_LENGTH_PRESETS_S, canStartCapture,
    captureProgress, captureStateFromCode, captureStateLabel, captureStateTone,
    effectiveLimitSeconds, formatCaptureDuration,
} from "@/services/capture";
import { Link, useRouter } from "expo-router";
import React from "react";
import { Pressable, ScrollView, StyleSheet, View } from "react-native";
import { SafeAreaView } from "react-native-safe-area-context";

function decodeUint32OrNull(encoded: string | null | undefined): number | null {
    if (!encoded) return null;
    try {
        return decodeUint32FromBase64(encoded);
    } catch {
        return null;
    }
}

/**
 * Capture screen: start and stop an on-device audio + IMU recording from the phone,
 * so a real stimulus can be captured in the field and turned into a simulator
 * scenario later (issue #53 tooling, fw/tools/capture_to_scenario.py).
 *
 * The phone deliberately never downloads anything. Captures accumulate on the
 * device's FAT volume under auto-indexed names and are collected over USB mass
 * storage — which is why the two numbers this screen keeps in front of the user are
 * "how many are waiting" and "how much room is left", not a file list.
 */
export default function CaptureScreen() {
    const router = useRouter();
    const { selectedDevice, writeToCharacteristic } = useBluetooth();
    const c = useThemeColors();
    // Issue #248: a live disconnect pops back to the Connect tab rather than
    // leaving a dead screen up; the EmptyState below is the render-time fallback.
    useDisconnectRedirect();

    // State and Elapsed are notifiable and pushed by the device (Elapsed at 1 Hz
    // while recording); Remaining and Count are read-only — they change when files
    // are added or REMOVED over USB, which the device cannot observe live, so the
    // hook polls them instead. Subscribing only while this screen is focused is what
    // keeps the app inside Android's ~15-slot registration budget.
    useScopedCharacteristicMonitors(React.useMemo(() => [
        { serviceUuid: UUID_CAPTURE_SERVICE, charUuid: UUID_CAPTURE_STATE },
        { serviceUuid: UUID_CAPTURE_SERVICE, charUuid: UUID_CAPTURE_ELAPSED_S },
        { serviceUuid: UUID_CAPTURE_SERVICE, charUuid: UUID_CAPTURE_REMAINING_S },
        { serviceUuid: UUID_CAPTURE_SERVICE, charUuid: UUID_CAPTURE_COUNT },
    ], []));

    // Covers the window between tapping Record and the device's own state
    // notification landing. Cleared by the write settling — a failed start (no room,
    // already running) surfaces through the control's WriteErrorIndicator, so this
    // only has to stop the button from looking dead, never to report an outcome.
    const [commandInFlight, setCommandInFlight] = React.useState(false);

    const chars = selectedDevice?.characteristics;
    const controlInfo = chars?.[UUID_CAPTURE_CONTROL] ?? null;
    const limitInfo = chars?.[UUID_CAPTURE_LIMIT_S] ?? null;

    const state = captureStateFromCode(decodeUint32OrNull(chars?.[UUID_CAPTURE_STATE]?.value));
    const elapsedS = decodeUint32OrNull(chars?.[UUID_CAPTURE_ELAPSED_S]?.value);
    const remainingS = decodeUint32OrNull(chars?.[UUID_CAPTURE_REMAINING_S]?.value);
    const count = decodeUint32OrNull(chars?.[UUID_CAPTURE_COUNT]?.value);
    const limitS = decodeUint32OrNull(limitInfo?.value);
    const willRecordS = effectiveLimitSeconds(limitS, remainingS);
    const recording = state === 'recording';

    const sendCommand = React.useCallback(async (command: number) => {
        setCommandInFlight(true);
        try {
            // No optimistic update: the control is a command, not a value — showing
            // "1" as its state would claim a capture started before the device has
            // accepted it, and the device's own Capture State is the truth here.
            await writeToCharacteristic(UUID_CAPTURE_CONTROL, encodeUint32ToBase64(command),
                                        { skipOptimisticUpdate: true });
        } finally {
            setCommandInFlight(false);
        }
    }, [writeToCharacteristic]);

    const setLimit = React.useCallback((seconds: number) => {
        writeToCharacteristic(UUID_CAPTURE_LIMIT_S, encodeUint32ToBase64(seconds));
    }, [writeToCharacteristic]);

    const header = (
        <View style={styles.header}>
            <Pressable
                onPress={() => router.back()}
                accessibilityRole="button"
                accessibilityLabel="Back to Controls"
                hitSlop={8}
                style={styles.backButton}
            >
                <IconSymbol name="chevron.left" size={22} color={c.primary} />
                <ThemedText style={{ color: c.primary }}>Controls</ThemedText>
            </Pressable>
        </View>
    );

    if (selectedDevice == null || controlInfo == null) {
        return (
            <SafeAreaView style={[styles.container, { backgroundColor: c.background }]} edges={['top']}>
                {header}
                <EmptyState
                    icon="🎙️"
                    title="Not available"
                    subtitle="This device isn't connected, or its firmware doesn't expose the capture service."
                    action={
                        <Link href="/(tabs)/device-state" asChild>
                            <AppButton title="Back to Controls" variant="primary" />
                        </Link>
                    }
                />
            </SafeAreaView>
        );
    }

    return (
        <SafeAreaView style={[styles.container, { backgroundColor: c.background }]} edges={['top']}>
            {header}
            <ScrollView contentContainerStyle={styles.scrollContent}>
                <ThemedText type="heading">Capture</ThemedText>

                <Card style={styles.card}>
                    <Section>
                        <View style={styles.statusRow}>
                            <ThemedText type="defaultSemiBold">Recorder</ThemedText>
                            <View style={styles.statusRight}>
                                <WriteErrorIndicator charInfo={controlInfo} />
                                <Badge label={captureStateLabel(state)} tone={captureStateTone(state)} />
                            </View>
                        </View>

                        {recording ? (
                            <>
                                <ProgressBar
                                    progress={captureProgress(elapsedS, willRecordS)}
                                    label={`${formatCaptureDuration(elapsedS)} of ${formatCaptureDuration(willRecordS)}`}
                                />
                                <AppButton
                                    title="Stop"
                                    variant="danger"
                                    disabled={commandInFlight}
                                    onPress={() => sendCommand(CAPTURE_COMMAND_STOP)}
                                    accessibilityLabel="Stop recording"
                                />
                            </>
                        ) : (
                            <>
                                <ThemedText type="caption">
                                    {canStartCapture(remainingS)
                                        ? `Records up to ${formatCaptureDuration(willRecordS)} of audio + IMU, then stops on its own.`
                                        : "No room left on the device — collect and delete the existing captures first."}
                                </ThemedText>
                                <AppButton
                                    title={state === 'failed' ? "Record again" : "Record"}
                                    variant="primary"
                                    disabled={commandInFlight || !canStartCapture(remainingS)}
                                    onPress={() => sendCommand(CAPTURE_COMMAND_START)}
                                    accessibilityLabel="Start recording"
                                />
                            </>
                        )}
                    </Section>
                </Card>

                {limitInfo && (
                    <Card style={styles.card}>
                        <Section title="Length" subtitle="Applied to the next capture; a running one can be stopped any time.">
                            <View style={styles.limitRow}>
                                <WriteErrorIndicator charInfo={limitInfo} />
                                <SegmentedControl
                                    options={CAPTURE_LENGTH_PRESETS_S.map(seconds => ({
                                        label: formatCaptureDuration(seconds),
                                        value: seconds,
                                    }))}
                                    value={limitS ?? 0}
                                    onChange={setLimit}
                                />
                            </View>
                            {limitS != null && willRecordS != null && willRecordS < limitS && (
                                <ThemedText type="caption">
                                    {`Only ${formatCaptureDuration(willRecordS)} will fit — the device stops there.`}
                                </ThemedText>
                            )}
                        </Section>
                    </Card>
                )}

                <Card style={styles.card}>
                    <Section title="Storage" subtitle="Captures stay on the device; collect them over USB.">
                        <ListRow label="Captures on device">
                            <ThemedText type="defaultSemiBold">{count == null ? '—' : String(count)}</ThemedText>
                        </ListRow>
                        <Divider />
                        <ListRow label="Room remaining">
                            <ThemedText type="defaultSemiBold">{formatCaptureDuration(remainingS)}</ThemedText>
                        </ListRow>
                    </Section>
                </Card>
            </ScrollView>
        </SafeAreaView>
    );
}

const styles = StyleSheet.create({
    container: {
        flex: 1,
    },
    header: {
        paddingHorizontal: Spacing.lg,
        paddingTop: Spacing.lg,
        paddingBottom: Spacing.sm,
    },
    backButton: {
        flexDirection: 'row',
        alignItems: 'center',
        gap: Spacing.xs,
        alignSelf: 'flex-start',
    },
    scrollContent: {
        paddingHorizontal: Spacing.lg,
        paddingBottom: Spacing.xxl,
        gap: Spacing.md,
    },
    card: {
        marginBottom: 0,
    },
    statusRow: {
        flexDirection: 'row',
        alignItems: 'center',
        justifyContent: 'space-between',
        gap: Spacing.sm,
    },
    statusRight: {
        flexDirection: 'row',
        alignItems: 'center',
        gap: Spacing.sm,
    },
    limitRow: {
        flexDirection: 'row',
        alignItems: 'center',
        gap: Spacing.sm,
    },
});
