import { LinearGradient } from 'expo-linear-gradient';
import { ReactNode } from 'react';
import { Pressable, PressableProps, StyleProp, StyleSheet, Text, View, ViewStyle } from 'react-native';

import { Gradients, Radii, Spacing } from '@/constants/theme';
import { useThemeColors } from '@/hooks/use-theme-color';

type Variant = 'primary' | 'secondary' | 'danger' | 'ghost';

interface Props extends PressableProps {
  title?: string;
  variant?: Variant;
  /** Custom content instead of a text label. */
  children?: ReactNode;
  style?: StyleProp<ViewStyle>;
}

/**
 * Pressable button with gradient/solid/ghost variants.
 *
 * Spreads `...rest` onto the Pressable so `<Link asChild>` can inject its `onPress`.
 * Intentionally has NO built-in spinner — callers that need a loading indicator render
 * their own (see bluetooth-device-list-item, where a separate ActivityIndicator is asserted).
 */
export function AppButton({ title, variant = 'primary', disabled = false, children, style, ...rest }: Props) {
  const c = useThemeColors();

  const labelColor =
    variant === 'primary' || variant === 'danger'
      ? c.onPrimary
      : variant === 'ghost'
        ? c.primary
        : c.textPrimary;

  const label =
    children ?? (
      <Text style={[styles.label, { color: labelColor }]} numberOfLines={1}>
        {title}
      </Text>
    );

  const inner =
    variant === 'primary' ? (
      <LinearGradient colors={Gradients.primary} start={{ x: 0, y: 0 }} end={{ x: 1, y: 0 }} style={styles.fill}>
        {label}
      </LinearGradient>
    ) : (
      <View
        style={[
          styles.fill,
          variant === 'secondary' && { backgroundColor: c.surfaceAlt, borderColor: c.border, borderWidth: 1 },
          variant === 'danger' && { backgroundColor: c.danger },
          variant === 'ghost' && styles.ghost,
        ]}
      >
        {label}
      </View>
    );

  return (
    <Pressable
      accessibilityRole="button"
      accessibilityLabel={title}
      accessibilityState={{ disabled: !!disabled }}
      disabled={disabled}
      style={[styles.base, disabled && styles.disabled, style]}
      {...rest}
    >
      {inner}
    </Pressable>
  );
}

const styles = StyleSheet.create({
  base: { borderRadius: Radii.md, overflow: 'hidden' },
  disabled: { opacity: 0.5 },
  fill: {
    minHeight: 44,
    paddingVertical: Spacing.sm + 2,
    paddingHorizontal: Spacing.lg,
    alignItems: 'center',
    justifyContent: 'center',
    borderRadius: Radii.md,
  },
  ghost: { backgroundColor: 'transparent' },
  label: { fontSize: 16, fontWeight: '600' },
});
