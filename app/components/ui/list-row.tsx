import { ReactNode } from 'react';
import { Animated, StyleSheet, View } from 'react-native';

import { ThemedText } from '@/components/themed-text';
import { Spacing } from '@/constants/theme';
import { useThemeColors } from '@/hooks/use-theme-color';

interface Props {
  label: string;
  sublabel?: string;
  /** Optional animated/static color for the label (used for write-status feedback). */
  labelColor?: string | Animated.AnimatedInterpolation<string | number>;
  children?: ReactNode;
}

/** A label + right-aligned control row. The label is an Animated.Text so callers can fade its color. */
export function ListRow({ label, sublabel, labelColor, children }: Props) {
  const c = useThemeColors();
  return (
    <View style={styles.row}>
      <View style={styles.labelWrap}>
        <Animated.Text style={[styles.label, { color: labelColor ?? c.textPrimary }]} numberOfLines={2}>
          {label}
        </Animated.Text>
        {sublabel ? <ThemedText type="caption">{sublabel}</ThemedText> : null}
      </View>
      {children ? <View style={styles.control}>{children}</View> : null}
    </View>
  );
}

const styles = StyleSheet.create({
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: Spacing.md,
    paddingVertical: Spacing.sm,
  },
  labelWrap: { flexShrink: 1, gap: 2 },
  label: { fontSize: 14, fontWeight: '500' },
  control: { flex: 1, flexDirection: 'row', alignItems: 'center', justifyContent: 'flex-end', gap: Spacing.sm },
});
