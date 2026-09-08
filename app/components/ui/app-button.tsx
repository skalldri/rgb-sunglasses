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
 * The vertical size the caller put on the outer Pressable, to be repeated on the visible fill.
 * `style` lands on the Pressable (the touch target), so without this a `minHeight: 56` caller
 * would get a 56 dp target wrapped around a 44 dp surface. Exported for the unit test.
 */
export function verticalSizeOf(style: StyleProp<ViewStyle>): Pick<ViewStyle, 'height' | 'minHeight' | 'maxHeight'> {
  const flat = StyleSheet.flatten(style) ?? {};
  const out: Pick<ViewStyle, 'height' | 'minHeight' | 'maxHeight'> = {};
  if (flat.height != null) out.height = flat.height;
  if (flat.minHeight != null) out.minHeight = flat.minHeight;
  if (flat.maxHeight != null) out.maxHeight = flat.maxHeight;
  return out;
}

/**
 * Pressable button with gradient/solid/ghost variants.
 *
 * Spreads `...rest` onto the Pressable so `<Link asChild>` can inject its `onPress` — and
 * so `testID` reaches the touchable itself rather than a wrapper. Don't destructure
 * `testID` out: /drive-app depends on it landing on the Pressable, because a tap
 * resolved to a wrapper View presses nothing and still reports success.
 * Intentionally has NO built-in spinner — callers that need a loading indicator render
 * their own (see bluetooth-device-list-item, where a separate ActivityIndicator is asserted).
 */
export function AppButton({ title, variant = 'primary', disabled = false, children, style, ...rest }: Props) {
  const c = useThemeColors();
  const fillSize = verticalSizeOf(style);

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
      <LinearGradient colors={Gradients.primary} start={{ x: 0, y: 0 }} end={{ x: 1, y: 0 }} style={[styles.fill, fillSize]}>
        {label}
      </LinearGradient>
    ) : (
      <View
        style={[
          styles.fill,
          fillSize,
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
    /* NO flexGrow here. It was added so a caller asking for a 56 dp button (the Audio Tuning
     * footer) got a 56 dp visible fill rather than a 44 dp fill inside a 56 dp touch target, on
     * the theory that it is inert in a container with no height of its own. On React Native's
     * Yoga it is not: the Pressable is measured with an "at most" height, and the legacy
     * StretchFlexBasis errata hands that whole budget to a flex-grow child. Anywhere a button is
     * not inside a ScrollView or a fixed-height row it filled the screen -- measured 2026-09-08
     * on the Controls tab's "Go to Connect" (Pixel 9 Pro, app-v3.5.0): 1676 px tall, ending
     * 165 px below the bottom of the display. The caller's vertical size is mirrored onto the
     * fill in `verticalSizeOf` instead, which is the only thing flexGrow was there to do. */
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
