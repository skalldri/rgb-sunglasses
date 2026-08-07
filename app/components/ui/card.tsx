import { ReactNode } from 'react';
import { StyleProp, StyleSheet, View, ViewStyle } from 'react-native';

import { Radii, Shadows, Spacing } from '@/constants/theme';
import { useThemeColors } from '@/hooks/use-theme-color';

interface Props {
  children: ReactNode;
  style?: StyleProp<ViewStyle>;
  padded?: boolean;
  /** Forwarded to the underlying View. Load-bearing for /drive-app — a card is how a
   *  hardware validation run reads which step a screen is on. */
  testID?: string;
}

/** Themed surface container with rounded corners, hairline border and a soft shadow. */
export function Card({ children, style, padded = true, testID }: Props) {
  const c = useThemeColors();
  return (
    <View
      testID={testID}
      style={[
        styles.card,
        { backgroundColor: c.surface, borderColor: c.border },
        padded && styles.padded,
        style,
      ]}
    >
      {children}
    </View>
  );
}

const styles = StyleSheet.create({
  card: { borderRadius: Radii.lg, borderWidth: 1, ...Shadows.card },
  padded: { padding: Spacing.lg },
});
