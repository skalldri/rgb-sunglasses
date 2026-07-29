import { Pressable, StyleSheet, View } from 'react-native';

import { ThemedText } from '@/components/themed-text';
import { Radii, Spacing } from '@/constants/theme';
import { useThemeColors } from '@/hooks/use-theme-color';

interface Props<T extends string | number> {
  options: { label: string; value: T }[];
  value: T;
  onChange: (value: T) => void;
}

/**
 * Wrapping row of selectable pills (chip group). Used instead of an iOS-style
 * fixed-width segmented bar because option labels here can be long (e.g. the
 * color-mode names) and the set can wrap to multiple lines on a narrow card.
 */
export function SegmentedControl<T extends string | number>({ options, value, onChange }: Props<T>) {
  const c = useThemeColors();
  return (
    <View style={styles.row}>
      {options.map((option) => {
        const selected = option.value === value;
        return (
          <Pressable
            key={String(option.value)}
            accessibilityRole="button"
            accessibilityState={{ selected }}
            onPress={() => onChange(option.value)}
            style={[
              styles.pill,
              selected
                ? { backgroundColor: c.primary, borderColor: c.primary }
                : { backgroundColor: c.surfaceAlt, borderColor: c.border },
            ]}
          >
            <ThemedText
              style={[styles.label, { color: selected ? c.onPrimary : c.textSecondary }]}
            >
              {option.label}
            </ThemedText>
          </Pressable>
        );
      })}
    </View>
  );
}

const styles = StyleSheet.create({
  row: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: Spacing.xs,
    justifyContent: 'center',
  },
  pill: {
    borderRadius: Radii.pill,
    borderWidth: 1,
    paddingHorizontal: Spacing.sm,
    paddingVertical: 6,
  },
  label: { fontSize: 13, fontWeight: '600' },
});
