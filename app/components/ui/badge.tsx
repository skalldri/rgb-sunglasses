import { StyleSheet, View } from 'react-native';

import { ThemedText } from '@/components/themed-text';
import { Radii, Spacing } from '@/constants/theme';
import { useThemeColors } from '@/hooks/use-theme-color';

type Tone = 'neutral' | 'success' | 'danger' | 'warning' | 'info';

interface Props {
  label: string;
  tone?: Tone;
}

/** Small rounded status pill, tinted from a semantic token. */
export function Badge({ label, tone = 'neutral' }: Props) {
  const c = useThemeColors();
  const color =
    tone === 'success'
      ? c.success
      : tone === 'danger'
        ? c.danger
        : tone === 'warning'
          ? c.warning
          : tone === 'info'
            ? c.info
            : c.textSecondary;
  return (
    <View style={[styles.badge, { backgroundColor: color + '26', borderColor: color + '55' }]}>
      <ThemedText style={[styles.text, { color }]}>{label}</ThemedText>
    </View>
  );
}

const styles = StyleSheet.create({
  badge: {
    alignSelf: 'flex-start',
    borderRadius: Radii.pill,
    borderWidth: 1,
    paddingHorizontal: Spacing.sm,
    paddingVertical: 2,
  },
  text: { fontSize: 12, fontWeight: '700' },
});
