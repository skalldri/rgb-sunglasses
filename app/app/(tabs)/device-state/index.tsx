import { CharacteristicBoolean } from "@/components/characteristic-boolean";
import { ThemedText } from "@/components/themed-text";
import { AppButton } from "@/components/ui/app-button";
import { Badge } from "@/components/ui/badge";
import { Card } from "@/components/ui/card";
import { Divider } from "@/components/ui/divider";
import { EmptyState } from "@/components/ui/empty-state";
import { IconSymbol } from "@/components/ui/icon-symbol";
import { Section } from "@/components/ui/section";
import { getServiceName, UUID_GENERIC_ACCESS_SERVICE, UUID_GENERIC_ATTRIBUTE_SERVICE, UUID_IS_ACTIVE_CHARACTERISTIC } from "@/constants/bluetooth";
import { Spacing } from "@/constants/theme";
import { useBluetooth } from "@/context/bluetooth-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import { SMP_SERVICE_UUID } from "@/services/mcumgr";
import { Link } from "expo-router";
import React, { ReactNode } from "react";
import { Pressable, ScrollView, StyleSheet, View } from "react-native";
import { SafeAreaView } from "react-native-safe-area-context";

function MenuRow({ label, href, rightSlot }: { label: string; href: string; rightSlot?: ReactNode }) {
    const c = useThemeColors();
    return (
        <View style={styles.menuRow}>
            <Link href={href as never} asChild>
                <Pressable style={styles.menuRowTap}>
                    <ThemedText style={styles.menuRowLabel}>{label}</ThemedText>
                </Pressable>
            </Link>
            <View style={styles.menuRowRight}>
                {rightSlot}
                {/* Separate Pressable from the label's, so a Switch placed in rightSlot never sits
                    inside the same touch target as the navigation link (Switch-inside-Pressable
                    causes touch conflicts on Android). */}
                <Link href={href as never} asChild>
                    <Pressable hitSlop={8}>
                        <IconSymbol name="chevron.right" size={20} color={c.textMuted} />
                    </Pressable>
                </Link>
            </View>
        </View>
    );
}

export default function DeviceStateMenuScreen() {
    const { selectedDevice, writeServiceCharacteristic } = useBluetooth();
    const c = useThemeColors();

    const visibleServices = (selectedDevice?.services ?? [])
        .filter(service => service.uuid !== UUID_GENERIC_ATTRIBUTE_SERVICE && service.uuid !== UUID_GENERIC_ACCESS_SERVICE);

    const firmwareService = visibleServices.find(service => service.uuid === SMP_SERVICE_UUID);
    // Animations are never hardcoded: a service is "an animation" purely because the firmware
    // exposed a live UUID_ANIMATION_NAME_CHARACTERISTIC for it (see serviceDisplayNames).
    const animationServices = visibleServices.filter(
        service => service.uuid !== SMP_SERVICE_UUID && selectedDevice?.serviceDisplayNames?.[service.uuid] != null
    );
    const settingsServices = visibleServices.filter(
        service => service.uuid !== SMP_SERVICE_UUID && selectedDevice?.serviceDisplayNames?.[service.uuid] == null
    );

    return (
        <SafeAreaView style={[styles.container, { backgroundColor: c.background }]} edges={['top']}>
            <View style={styles.header}>
                <ThemedText type="heading">Controls</ThemedText>
                <Badge
                    label={selectedDevice == null ? "NOT CONNECTED" : selectedDevice.name}
                    tone={selectedDevice == null ? 'danger' : 'success'}
                />
            </View>

            {selectedDevice == null ? (
                <EmptyState
                    icon="🕶️"
                    title="Not connected"
                    subtitle="Open the Connect tab to pair your glasses."
                    action={
                        <Link href="/(tabs)/bluetooth" asChild>
                            <AppButton title="Go to Connect" variant="primary" />
                        </Link>
                    }
                />
            ) : (
                <ScrollView contentContainerStyle={styles.scrollContent}>
                    {firmwareService && (
                        <Card style={styles.card}>
                            <Section title="Firmware">
                                <MenuRow label="Firmware Update" href="/firmware-update-modal" />
                            </Section>
                        </Card>
                    )}

                    {animationServices.length > 0 && (
                        <Card style={styles.card}>
                            <Section title="Animations">
                                {animationServices.map((service, index) => {
                                    const isActiveInfo = selectedDevice.characteristicsByService[service.uuid]?.[UUID_IS_ACTIVE_CHARACTERISTIC] ?? null;
                                    return (
                                        <React.Fragment key={service.uuid}>
                                            {index > 0 && <Divider />}
                                            <MenuRow
                                                label={selectedDevice.serviceDisplayNames?.[service.uuid] ?? getServiceName(service.uuid)}
                                                href={`/(tabs)/device-state/${service.uuid}`}
                                                rightSlot={isActiveInfo && (
                                                    <CharacteristicBoolean
                                                        charUuid={UUID_IS_ACTIVE_CHARACTERISTIC}
                                                        charInfo={isActiveInfo}
                                                        onWrite={(charUuid, encoded) => writeServiceCharacteristic(service.uuid, charUuid, encoded)}
                                                    />
                                                )}
                                            />
                                        </React.Fragment>
                                    );
                                })}
                            </Section>
                        </Card>
                    )}

                    {settingsServices.length > 0 && (
                        <Card style={styles.card}>
                            <Section title="Settings">
                                {settingsServices.map((service, index) => (
                                    <React.Fragment key={service.uuid}>
                                        {index > 0 && <Divider />}
                                        <MenuRow
                                            label={getServiceName(service.uuid)}
                                            href={`/(tabs)/device-state/${service.uuid}`}
                                        />
                                    </React.Fragment>
                                ))}
                            </Section>
                        </Card>
                    )}
                </ScrollView>
            )}
        </SafeAreaView>
    );
}

const styles = StyleSheet.create({
    container: {
        flex: 1,
    },
    header: {
        flexDirection: 'row',
        alignItems: 'center',
        justifyContent: 'space-between',
        gap: Spacing.sm,
        paddingHorizontal: Spacing.lg,
        paddingTop: Spacing.lg,
        paddingBottom: Spacing.md,
    },
    scrollContent: {
        paddingHorizontal: Spacing.lg,
        paddingBottom: Spacing.xxl,
        gap: Spacing.md,
    },
    card: {
        marginBottom: 0,
    },
    menuRow: {
        flexDirection: 'row',
        alignItems: 'center',
        justifyContent: 'space-between',
        gap: Spacing.sm,
    },
    menuRowTap: {
        flex: 1,
        paddingVertical: Spacing.sm,
    },
    menuRowLabel: {
        fontSize: 16,
    },
    menuRowRight: {
        flexDirection: 'row',
        alignItems: 'center',
        gap: Spacing.sm,
    },
});
