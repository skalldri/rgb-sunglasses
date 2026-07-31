import { CharacteristicSlotRow } from "@/components/characteristic-slot-row";
import { WriteErrorIndicator } from "@/components/characteristic-write-error";
import { ThemedText } from "@/components/themed-text";
import { AppButton } from "@/components/ui/app-button";
import { Card } from "@/components/ui/card";
import { Divider } from "@/components/ui/divider";
import { EmptyState } from "@/components/ui/empty-state";
import { IconSymbol } from "@/components/ui/icon-symbol";
import { ListRow } from "@/components/ui/list-row";
import { Section } from "@/components/ui/section";
import { getCharacteristicName, getServiceName, UUID_ANIMATION_NAME_CHARACTERISTIC } from "@/constants/bluetooth";
import { Spacing } from "@/constants/theme";
import { useBluetooth } from "@/context/bluetooth-context";
import { useCharacteristicEditor } from "@/hooks/use-characteristic-editor";
import { useDisconnectRedirect } from "@/hooks/use-disconnect-redirect";
import { useThemeColors } from "@/hooks/use-theme-color";
import { encodeUint32ToBase64 } from "@/services/ble-value-codec";
import { SMP_CHARACTERISTIC_UUID, SMP_SERVICE_UUID } from "@/services/mcumgr";
import { decodeSlotIndex, groupSlotPlaylist, isServiceActive } from "@/services/slot-playlist";
import { Link, useLocalSearchParams, useRouter } from "expo-router";
import React from "react";
import { Pressable, ScrollView, StyleSheet, View } from "react-native";
import { SafeAreaView } from "react-native-safe-area-context";

export default function DeviceStateDetailScreen() {
    const params = useLocalSearchParams();
    const serviceUuid = params.serviceUuid as string;
    const router = useRouter();
    const { selectedDevice, writeToCharacteristic } = useBluetooth();
    const { renderCharacteristicInput, labelColorFor } = useCharacteristicEditor();
    const c = useThemeColors();
    // Issue #248: on disconnect, pop this screen (and the stack) back to the
    // Connect tab; the EmptyState below stays as a render-time fallback only.
    useDisconnectRedirect();

    const serviceCharacteristics = selectedDevice?.characteristicsByService?.[serviceUuid] ?? null;
    const title = selectedDevice?.serviceDisplayNames?.[serviceUuid] ?? getServiceName(serviceUuid);

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

    if (selectedDevice == null || serviceCharacteristics == null) {
        return (
            <SafeAreaView style={[styles.container, { backgroundColor: c.background }]} edges={['top']}>
                {header}
                <EmptyState
                    icon="🕶️"
                    title="Not available"
                    subtitle="This device isn't connected, or no longer exposes this service."
                    action={
                        <Link href="/(tabs)/device-state" asChild>
                            <AppButton title="Back to Controls" variant="primary" />
                        </Link>
                    }
                />
            </SafeAreaView>
        );
    }

    // Slot playlist (issue #260): services exposing SLOT_TEXT characteristics get a
    // dedicated "Slots" section (tap-to-queue + now-playing highlight); the raw Up Next /
    // Now Playing characteristics are absorbed into it instead of rendering as rows.
    // Services without slot CPFs get null here and render exactly as before.
    const slotPlaylist = groupSlotPlaylist(serviceCharacteristics);

    const characteristics = Object.entries(serviceCharacteristics)
        .filter(([charUuid]) => charUuid !== UUID_ANIMATION_NAME_CHARACTERISTIC &&
            !slotPlaylist?.hiddenCharUuids.has(charUuid));

    const upNextInfo = slotPlaylist?.upNext?.charInfo ?? null;
    const upNextIndex = decodeSlotIndex(upNextInfo);
    // Now Playing holds its last value while the animation is off (and defaults to 0 at
    // boot), so the "this is on the glasses right now" highlight is additionally gated
    // on the service's Is Active characteristic. The up-next tint is NOT gated — a
    // queued slot is meaningful while inactive (it plays on the next activation).
    const serviceIsActive = slotPlaylist !== null && isServiceActive(serviceCharacteristics);
    const nowPlayingIndex = serviceIsActive
        ? decodeSlotIndex(slotPlaylist?.nowPlaying?.charInfo)
        : null;

    // Queue a slot: write its index to the service's SLOT_UP_NEXT characteristic (unique
    // per-service auto UUID, so the flat write path is correct — unlike is-active/
    // shuffle-include there's no cross-service UUID reuse). Default optimistic options:
    // the highlight jumps immediately, and the PR #98 compare-and-swap revert keeps an
    // autonomous-advance notify arriving mid-write from being clobbered.
    const queueSlot = (slotIndex: number) => {
        if (!slotPlaylist?.upNext) return;
        writeToCharacteristic(slotPlaylist.upNext.charUuid, encodeUint32ToBase64(slotIndex));
    };

    return (
        <SafeAreaView style={[styles.container, { backgroundColor: c.background }]} edges={['top']}>
            {header}
            {/* keyboardShouldPersistTaps, ONLY on slot-playlist screens: with a slot's
                keyboard open, the first tap on an up-next button must fire the button,
                not just dismiss the keyboard. Every other service screen keeps the stock
                first-tap-dismisses behavior so a keyboard-dismissing tap can't
                accidentally fire a toggle and send an unintended BLE write. */}
            <ScrollView
                contentContainerStyle={styles.scrollContent}
                keyboardShouldPersistTaps={slotPlaylist ? "handled" : undefined}
            >
                <ThemedText type="heading">{title}</ThemedText>
                <Card style={styles.card}>
                    <Section>
                        {characteristics.map(([charUuid, charInfo], charIndex) => {
                            const isMcuMgrCharacteristic = serviceUuid === SMP_SERVICE_UUID && charUuid === SMP_CHARACTERISTIC_UUID;

                            return (
                                <React.Fragment key={`${serviceUuid}-char-${charIndex}`}>
                                    {charIndex > 0 && <Divider />}
                                    <ListRow
                                        label={charInfo.name ?? getCharacteristicName(charUuid)}
                                        labelColor={labelColorFor(charUuid)}
                                    >
                                        <WriteErrorIndicator charInfo={charInfo} />
                                        {isMcuMgrCharacteristic && (
                                            <Link href="/firmware-update-modal" asChild>
                                                <AppButton title="Update" variant="secondary" />
                                            </Link>
                                        )}
                                        {renderCharacteristicInput(serviceUuid, charUuid, charInfo)}
                                    </ListRow>
                                </React.Fragment>
                            );
                        })}
                    </Section>
                    {slotPlaylist && (
                        <Section title="Slots" right={upNextInfo ? <WriteErrorIndicator charInfo={upNextInfo} /> : undefined}>
                            {slotPlaylist.slots.map(({ charUuid, charInfo, slotIndex }) => (
                                <React.Fragment key={`${serviceUuid}-slot-${slotIndex}`}>
                                    {slotIndex > 0 && <Divider />}
                                    <CharacteristicSlotRow
                                        label={charInfo.name ?? getCharacteristicName(charUuid)}
                                        labelColor={labelColorFor(charUuid)}
                                        slotIndex={slotIndex}
                                        isNowPlaying={nowPlayingIndex !== null && nowPlayingIndex === slotIndex}
                                        isUpNext={upNextIndex !== null && upNextIndex === slotIndex}
                                        showUpNextButton={slotPlaylist.upNext !== null}
                                        upNextDisabled={upNextInfo?.isUpdateInProgress ?? false}
                                        onQueueUpNext={() => queueSlot(slotIndex)}
                                    >
                                        <WriteErrorIndicator charInfo={charInfo} />
                                        {renderCharacteristicInput(serviceUuid, charUuid, charInfo)}
                                    </CharacteristicSlotRow>
                                </React.Fragment>
                            ))}
                        </Section>
                    )}
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
});
