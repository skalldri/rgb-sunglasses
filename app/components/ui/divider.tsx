import { StyleProp, StyleSheet, View, ViewStyle } from 'react-native';

import { useThemeColors } from '@/hooks/use-theme-color';

/** Themed hairline separator. Replaces hard-coded `#ccc` lines. */
export function Divider({ style }: { style?: StyleProp<ViewStyle> }) {
  const c = useThemeColors();
  return <View style={[styles.line, { backgroundColor: c.border }, style]} />;
}

const styles = StyleSheet.create({
  line: { height: StyleSheet.hairlineWidth, width: '100%' },
});
