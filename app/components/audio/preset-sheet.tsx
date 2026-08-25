import { useEffect, useRef, useState } from "react";
import {
  KeyboardAvoidingView,
  Modal,
  Platform,
  Pressable,
  ScrollView,
  StyleSheet,
  TextInput,
  View,
} from "react-native";

import { ThemedText } from "@/components/themed-text";
import { AppButton } from "@/components/ui/app-button";
import { Divider } from "@/components/ui/divider";
import { Radii, Spacing } from "@/constants/theme";
import { useThemeColors } from "@/hooks/use-theme-color";
import { AudioPreset } from "@/services/audio-presets";

interface Props {
  visible: boolean;
  presets: AudioPreset[];
  slotA: string | null;
  slotB: string | null;
  /** How many parameters applying each preset would change, keyed by preset id. */
  changeCounts: Record<string, number>;
  busy: boolean;
  suggestedName: string;
  onApply: (preset: AudioPreset) => void;
  onAssignSlot: (slot: "A" | "B", id: string) => void;
  onDelete: (id: string) => void;
  onSave: (name: string) => void;
  onClose: () => void;
}

/**
 * Preset picker, A/B slot assignment, and "save current as".
 *
 * The name field is the only keyboard on the whole Audio Tuning flow, and it is optional — it
 * comes pre-filled with a timestamped suggestion so the common path is tap-tap-done in a dark
 * room. Everything else is a button.
 */
export function PresetSheet({
  visible,
  presets,
  slotA,
  slotB,
  changeCounts,
  busy,
  suggestedName,
  onApply,
  onAssignSlot,
  onDelete,
  onSave,
  onClose,
}: Props) {
  const c = useThemeColors();
  const [name, setName] = useState(suggestedName);

  /* Re-seed the name every time the sheet OPENS, not once per app launch.
   *
   * A Modal's children stay mounted while `visible` is false, so `useState(suggestedName)` ran
   * exactly once and the field then held that first timestamp forever. Because saving
   * overwrites by name, the second save of a session silently replaced the first preset instead
   * of adding one — the user watched "Saved" appear and lost the earlier capture.
   *
   * The suggestion is read through a ref rather than a dependency: the parent recomputes it
   * from `new Date()` on every render, so depending on it directly would wipe the field out
   * from under anyone typing into it. */
  const suggestedRef = useRef(suggestedName);
  suggestedRef.current = suggestedName;
  useEffect(() => {
    if (visible) setName(suggestedRef.current);
  }, [visible]);

  return (
    <Modal
      visible={visible}
      transparent
      animationType="slide"
      onRequestClose={onClose}
    >
      {/* Without this the on-screen keyboard covers the Save button outright — the one
                keyboard on the whole Audio Tuning flow would make its own confirm button
                unreachable. Found on a Pixel 9 Pro; the name field sits at the bottom of a
                bottom-anchored sheet, which is exactly where the keyboard lands. */}
      <KeyboardAvoidingView
        style={styles.fill}
        behavior={Platform.OS === "ios" ? "padding" : "height"}
      >
        <Pressable
          style={[styles.backdrop, { backgroundColor: c.overlay }]}
          onPress={onClose}
        >
          <Pressable
            style={[
              styles.sheet,
              { backgroundColor: c.surface, borderColor: c.border },
            ]}
            onPress={() => {}}
            testID="preset-sheet"
          >
            <ThemedText type="heading">Presets</ThemedText>
            <ThemedText type="caption" style={{ color: c.textSecondary }}>
              Starting points, not answers — the room decides.
            </ThemedText>

            <ScrollView
              style={styles.list}
              contentContainerStyle={styles.listContent}
            >
              {presets.map((preset, index) => {
                const changes = changeCounts[preset.id] ?? 0;
                const isA = slotA === preset.id;
                const isB = slotB === preset.id;

                return (
                  <View key={preset.id}>
                    {index > 0 ? <Divider /> : null}
                    <View style={styles.row} testID={`preset-row-${preset.id}`}>
                      <View style={styles.rowText}>
                        <ThemedText style={styles.presetName}>
                          {preset.name}
                        </ThemedText>
                        {preset.blurb ? (
                          <ThemedText
                            type="caption"
                            style={{ color: c.textSecondary }}
                          >
                            {preset.blurb}
                          </ThemedText>
                        ) : null}
                        <ThemedText
                          type="caption"
                          style={{ color: c.textMuted }}
                        >
                          {changes === 0
                            ? "Already applied"
                            : `Changes ${changes} setting${changes === 1 ? "" : "s"}`}
                        </ThemedText>
                      </View>

                      <View style={styles.rowActions}>
                        <SlotButton
                          label="A"
                          active={isA}
                          onPress={() => onAssignSlot("A", preset.id)}
                          testID={`preset-slot-a-${preset.id}`}
                        />
                        <SlotButton
                          label="B"
                          active={isB}
                          onPress={() => onAssignSlot("B", preset.id)}
                          testID={`preset-slot-b-${preset.id}`}
                        />
                        {/* AppButton, not a fork of it: the hand-rolled copy
                            omitted accessibilityState, so a screen reader
                            announced a disabled Apply as tappable. Only the
                            pill radius is ours. */}
                        <AppButton
                          title="Apply"
                          accessibilityLabel={`Apply ${preset.name}`}
                          disabled={busy || changes === 0}
                          onPress={() => onApply(preset)}
                          testID={`preset-apply-${preset.id}`}
                          style={styles.applyButton}
                        />
                      </View>
                    </View>

                    {!preset.builtIn ? (
                      <Pressable
                        accessibilityRole="button"
                        accessibilityLabel={`Delete ${preset.name}`}
                        hitSlop={8}
                        onPress={() => onDelete(preset.id)}
                        testID={`preset-delete-${preset.id}`}
                      >
                        <ThemedText type="caption" style={{ color: c.danger }}>
                          Delete
                        </ThemedText>
                      </Pressable>
                    ) : null}
                  </View>
                );
              })}
            </ScrollView>

            <Divider />
            <ThemedText type="caption" style={{ color: c.textSecondary }}>
              Save what the glasses are set to right now
            </ThemedText>
            <View style={styles.saveRow}>
              <TextInput
                value={name}
                onChangeText={setName}
                placeholder={suggestedName}
                placeholderTextColor={c.textMuted}
                returnKeyType="done"
                onSubmitEditing={() => {
                  if (!busy && name.trim().length > 0) onSave(name.trim());
                }}
                testID="preset-save-name"
                style={[
                  styles.nameInput,
                  {
                    color: c.textPrimary,
                    borderColor: c.border,
                    backgroundColor: c.surfaceAlt,
                  },
                ]}
              />
              <AppButton
                title="Save"
                disabled={busy || name.trim().length === 0}
                onPress={() => onSave(name.trim())}
                testID="preset-save"
              />
            </View>

            <AppButton title="Done" variant="ghost" onPress={onClose} />
          </Pressable>
        </Pressable>
      </KeyboardAvoidingView>
    </Modal>
  );
}

function SlotButton({
  label,
  active,
  onPress,
  testID,
}: {
  label: string;
  active: boolean;
  onPress: () => void;
  testID: string;
}) {
  const c = useThemeColors();
  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel={`Assign to slot ${label}`}
      accessibilityState={{ selected: active }}
      hitSlop={8}
      onPress={onPress}
      testID={testID}
      style={[
        styles.slotButton,
        active
          ? { backgroundColor: c.info, borderColor: c.info }
          : { backgroundColor: "transparent", borderColor: c.border },
      ]}
    >
      <ThemedText
        style={{
          color: active ? c.onPrimary : c.textSecondary,
          fontWeight: "700",
        }}
      >
        {label}
      </ThemedText>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  fill: { flex: 1 },
  backdrop: { flex: 1, justifyContent: "flex-end" },
  sheet: {
    maxHeight: "88%",
    borderTopLeftRadius: Radii.lg,
    borderTopRightRadius: Radii.lg,
    borderWidth: 1,
    padding: Spacing.lg,
    gap: Spacing.sm,
  },
  list: { flexGrow: 0 },
  listContent: { paddingBottom: Spacing.xs },
  row: {
    flexDirection: "row",
    alignItems: "center",
    justifyContent: "space-between",
    gap: Spacing.md,
    paddingVertical: Spacing.md,
  },
  rowText: { flexShrink: 1, gap: 2 },
  presetName: { fontSize: 15, fontWeight: "600" },
  rowActions: { flexDirection: "row", alignItems: "center", gap: Spacing.sm },
  // 44 dp minimum: pressed one-handed, in the dark.
  slotButton: {
    width: 44,
    height: 44,
    borderRadius: Radii.pill,
    borderWidth: 1,
    alignItems: "center",
    justifyContent: "center",
  },
  applyButton: { borderRadius: Radii.pill },
  saveRow: { flexDirection: "row", alignItems: "center", gap: Spacing.sm },
  nameInput: {
    flex: 1,
    minHeight: 44,
    borderWidth: 1,
    borderRadius: Radii.md,
    paddingHorizontal: Spacing.md,
    fontSize: 15,
  },
});
