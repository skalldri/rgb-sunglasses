import { ReactNode } from 'react';
import { StyleSheet, View } from 'react-native';

import { ThemedText } from '@/components/themed-text';
import { Spacing } from '@/constants/theme';

interface Props {
  /** An emoji or short glyph shown above the title. */
  icon?: string;
  title: string;
  subtitle?: string;
  action?: ReactNode;
}

/** Centered placeholder for empty/disconnected states. */
export function EmptyState({ icon, title, subtitle, action }: Props) {
  return (
    <View style={styles.wrap}>
      {icon ? <ThemedText style={styles.icon}>{icon}</ThemedText> : null}
      <ThemedText type="subtitle" style={styles.center}>
        {title}
      </ThemedText>
      {subtitle ? (
        <ThemedText type="caption" style={styles.center}>
          {subtitle}
        </ThemedText>
      ) : null}
      {action ? <View style={styles.action}>{action}</View> : null}
    </View>
  );
}

const styles = StyleSheet.create({
  wrap: {
    alignItems: 'center',
    justifyContent: 'center',
    gap: Spacing.sm,
    paddingVertical: Spacing.xxl,
    paddingHorizontal: Spacing.lg,
  },
  icon: { fontSize: 48, lineHeight: 56 },
  center: { textAlign: 'center' },
  action: { marginTop: Spacing.md },
});
