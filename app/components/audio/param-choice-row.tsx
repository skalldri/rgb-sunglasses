import { memo } from "react";
import { Pressable, StyleSheet, View } from "react-native";

import { ThemedText } from "@/components/themed-text";
import { IconSymbol } from "@/components/ui/icon-symbol";
import { Radii, Spacing } from "@/constants/theme";
import { useThemeColors } from "@/hooks/use-theme-color";

export interface ChoiceOption {
  label: string;
  /** Shown under the pill row when this option is selected. */
  blurb?: string;
}

interface Props {
  title: string;
  /** Firmware name(s) this control drives, shown in grey in Advanced mode. */
  firmwareLabel?: string;
  options: ChoiceOption[];
  /**
   * Selected label, or null when the device's stored values do not match any option — which
   * renders a "Custom" pill rather than silently highlighting the nearest one. Lying about
   * which preset is active is worse than admitting the device is on something else.
   */
  selected: string | null;
  /** Description of the current custom value, e.g. "224 ms". */
  customLabel?: string | null;
  help: string;
  busy?: boolean;
  disabled?: boolean;
  onSelect: (label: string) => void;
  onHelp?: () => void;
  /**
   * Also used to namespace each option's testID. Two rows on this screen legitimately share
   * an option label ("Normal" is both a Beat feel and an adapt speed), so an unscoped
   * `choice-Normal` would be ambiguous to a test AND to an accessibility-strategy tap from
   * the /drive-app tooling on the physical phone.
   */
  testID?: string;
}

function ParamChoiceRowImpl({
  title,
  firmwareLabel,
  options,
  selected,
  customLabel,
  help,
  busy,
  disabled,
  onSelect,
  onHelp,
  testID,
}: Props) {
  const c = useThemeColors();
  const activeBlurb = options.find((o) => o.label === selected)?.blurb;

  return (
    <View style={styles.wrap} testID={testID}>
      <View style={styles.header}>
        <View style={styles.labelWrap}>
          <ThemedText style={styles.title}>{title}</ThemedText>
          {firmwareLabel ? (
            <ThemedText type="caption" style={{ color: c.textMuted }}>
              {firmwareLabel}
            </ThemedText>
          ) : null}
        </View>
        <View style={styles.headerRight}>
          {busy ? (
            <View style={[styles.busyDot, { backgroundColor: c.info }]} />
          ) : null}
          {onHelp ? (
            <Pressable
              accessibilityRole="button"
              accessibilityLabel={`What does ${title} do?`}
              hitSlop={12}
              onPress={onHelp}
              style={[styles.helpButton, { borderColor: c.border }]}
            >
              <IconSymbol
                name="questionmark"
                size={13}
                color={c.textSecondary}
              />
            </Pressable>
          ) : null}
        </View>
      </View>

      <View style={styles.pills}>
        {options.map((option) => {
          const isSelected = option.label === selected;
          return (
            <Pressable
              key={option.label}
              accessibilityRole="button"
              accessibilityState={{
                selected: isSelected,
                disabled: !!disabled,
              }}
              disabled={disabled}
              hitSlop={8}
              onPress={() => onSelect(option.label)}
              testID={`${testID ?? "choice"}-${option.label}`}
              style={[
                styles.pill,
                isSelected
                  ? { backgroundColor: c.primary, borderColor: c.primary }
                  : { backgroundColor: c.surfaceAlt, borderColor: c.border },
                disabled ? styles.pillDisabled : null,
              ]}
            >
              <ThemedText
                style={[
                  styles.pillLabel,
                  { color: isSelected ? c.onPrimary : c.textSecondary },
                ]}
              >
                {option.label}
              </ThemedText>
            </Pressable>
          );
        })}

        {selected === null ? (
          <View
            style={[styles.pill, styles.customPill, { borderColor: c.warning }]}
            testID={`${testID ?? "choice"}-custom`}
          >
            <ThemedText style={[styles.pillLabel, { color: c.warning }]}>
              {customLabel ? `Custom (${customLabel})` : "Custom"}
            </ThemedText>
          </View>
        ) : null}
      </View>

      <ThemedText type="caption" style={{ color: c.textSecondary }}>
        {activeBlurb
          ? `${help} ${activeBlurb.charAt(0).toUpperCase()}${activeBlurb.slice(1)}.`
          : help}
      </ThemedText>
    </View>
  );
}

export const ParamChoiceRow = memo(ParamChoiceRowImpl);

const styles = StyleSheet.create({
  wrap: { paddingVertical: Spacing.md, gap: Spacing.sm },
  header: {
    flexDirection: "row",
    alignItems: "center",
    justifyContent: "space-between",
    gap: Spacing.sm,
  },
  labelWrap: { flexShrink: 1, gap: 1 },
  headerRight: { flexDirection: "row", alignItems: "center", gap: Spacing.sm },
  title: { fontSize: 15, fontWeight: "600" },
  busyDot: { width: 6, height: 6, borderRadius: 3 },
  helpButton: {
    width: 22,
    height: 22,
    borderRadius: Radii.pill,
    borderWidth: 1,
    alignItems: "center",
    justifyContent: "center",
  },
  pills: { flexDirection: "row", flexWrap: "wrap", gap: Spacing.sm },
  // Deliberately tall: these are pressed one-handed, in the dark.
  pill: {
    minHeight: 44,
    justifyContent: "center",
    paddingHorizontal: Spacing.lg,
    borderRadius: Radii.pill,
    borderWidth: 1,
  },
  pillDisabled: { opacity: 0.4 },
  customPill: { borderStyle: "dashed" },
  pillLabel: { fontSize: 14, fontWeight: "600" },
});
