import { LinearGradient } from 'expo-linear-gradient';
import { StyleSheet } from 'react-native';

import { ThemedText } from '@/components/themed-text';
import { Gradients, Radii, Spacing } from '@/constants/theme';

interface Props {
  title: string;
  subtitle?: string;
  emoji?: string;
}

/** Brand gradient banner. Title text sits over the violet end (keep it short). */
export function Hero({ title, subtitle, emoji }: Props) {
  return (
    <LinearGradient colors={Gradients.brand} start={{ x: 0, y: 0 }} end={{ x: 1, y: 1 }} style={styles.hero}>
      {emoji ? <ThemedText style={styles.emoji}>{emoji}</ThemedText> : null}
      <ThemedText type="display" lightColor="#FFFFFF" darkColor="#FFFFFF" style={styles.title}>
        {title}
      </ThemedText>
      {subtitle ? (
        <ThemedText lightColor="#FFFFFF" darkColor="#FFFFFF" style={styles.subtitle}>
          {subtitle}
        </ThemedText>
      ) : null}
    </LinearGradient>
  );
}

const styles = StyleSheet.create({
  hero: { borderRadius: Radii.xl, padding: Spacing.xl, gap: Spacing.xs, overflow: 'hidden' },
  emoji: { fontSize: 40, lineHeight: 48 },
  title: { color: '#FFFFFF' },
  subtitle: { color: 'rgba(255,255,255,0.85)', fontSize: 15, fontWeight: '500' },
});
