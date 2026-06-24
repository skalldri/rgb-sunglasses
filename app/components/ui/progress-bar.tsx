import { LinearGradient } from 'expo-linear-gradient';
import { StyleSheet, View } from 'react-native';

import { ThemedText } from '@/components/themed-text';
import { Gradients, Spacing } from '@/constants/theme';
import { useThemeColors } from '@/hooks/use-theme-color';

interface Props {
  /** 0..1 */
  progress: number;
  label?: string;
  height?: number;
}

/** Gradient-filled progress bar on a themed track, with an optional caption label. */
export function ProgressBar({ progress, label, height = 8 }: Props) {
  const c = useThemeColors();
  const pct = Math.max(0, Math.min(1, progress)) * 100;
  return (
    <View style={styles.wrap}>
      <View style={[styles.track, { height, borderRadius: height / 2, backgroundColor: c.surfaceAlt }]}>
        <LinearGradient
          colors={Gradients.primary}
          start={{ x: 0, y: 0 }}
          end={{ x: 1, y: 0 }}
          style={[styles.fill, { width: `${pct}%`, borderRadius: height / 2 }]}
        />
      </View>
      {label ? <ThemedText type="caption">{label}</ThemedText> : null}
    </View>
  );
}

const styles = StyleSheet.create({
  wrap: { gap: Spacing.xs, width: '100%' },
  track: { width: '100%', overflow: 'hidden' },
  fill: { height: '100%' },
});
