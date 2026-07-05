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
import { useThemeColors } from "@/hooks/use-theme-color";
import { SMP_CHARACTERISTIC_UUID, SMP_SERVICE_UUID } from "@/services/mcumgr";
import { Link, useLocalSearchParams, useRouter } from "expo-router";
import React from "react";
import { Pressable, ScrollView, StyleSheet, View } from "react-native";
import { SafeAreaView } from "react-native-safe-area-context";

export default function DeviceStateDetailScreen() {
    const params = useLocalSearchParams();
    const serviceUuid = params.serviceUuid as string;
    const router = useRouter();
    const { selectedDevice } = useBluetooth();
    const { renderCharacteristicInput, labelColorFor } = useCharacteristicEditor();
    const c = useThemeColors();

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

    const characteristics = Object.entries(serviceCharacteristics)
        .filter(([charUuid]) => charUuid !== UUID_ANIMATION_NAME_CHARACTERISTIC);

    return (
        <SafeAreaView style={[styles.container, { backgroundColor: c.background }]} edges={['top']}>
            {header}
            <ScrollView contentContainerStyle={styles.scrollContent}>
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
