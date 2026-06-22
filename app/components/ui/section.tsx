import { ReactNode } from 'react';
import { StyleSheet, View } from 'react-native';

import { ThemedText } from '@/components/themed-text';
import { Spacing } from '@/constants/theme';

interface Props {
  title?: string;
  subtitle?: string;
  right?: ReactNode;
  children: ReactNode;
}

/** A titled group: an overline header (with optional right-aligned slot) above its children. */
export function Section({ title, subtitle, right, children }: Props) {
  return (
    <View style={styles.section}>
      {(title || right) && (
        <View style={styles.header}>
          <View style={styles.headerText}>
            {title ? <ThemedText type="overline">{title}</ThemedText> : null}
            {subtitle ? <ThemedText type="caption">{subtitle}</ThemedText> : null}
          </View>
          {right}
        </View>
      )}
      <View style={styles.body}>{children}</View>
    </View>
  );
}

const styles = StyleSheet.create({
  section: { gap: Spacing.sm },
  header: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', gap: Spacing.sm },
  headerText: { flexShrink: 1, gap: 2 },
  body: { gap: Spacing.md },
});
