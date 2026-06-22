/**
 * Design tokens for the RGB Sunglasses app.
 *
 * `Colors` holds the semantic palette for light & dark modes; `useThemeColor` /
 * `useThemeColors` (app/hooks/use-theme-color.ts) read from it based on the OS color scheme.
 * The non-color tokens (Spacing, Radii, Typography, Gradients, Shadows) are theme-independent.
 *
 * Legacy keys (`text`, `background`, `tint`, `icon`, `tabIconDefault`, `tabIconSelected`) are
 * kept so existing call sites keep working; new code should prefer the semantic keys.
 */

import { Platform } from 'react-native';

export const Colors = {
  light: {
    background: '#F7F7FB',
    surface: '#FFFFFF',
    surfaceAlt: '#EFEFF6',
    text: '#15151B',
    textPrimary: '#15151B',
    textSecondary: '#5B5B6B',
    textMuted: '#8A8A9A',
    border: '#E2E2EC',
    tint: '#7C3AED',
    primary: '#7C3AED',
    onPrimary: '#FFFFFF',
    success: '#16A34A',
    info: '#2563EB',
    danger: '#DC2626',
    warning: '#D97706',
    icon: '#5B5B6B',
    tabIconDefault: '#8A8A9A',
    tabIconSelected: '#7C3AED',
    overlay: 'rgba(15,15,25,0.45)',
  },
  dark: {
    background: '#0B0B12',
    surface: '#16161F',
    surfaceAlt: '#1F1F2B',
    text: '#ECECF4',
    textPrimary: '#ECECF4',
    textSecondary: '#A8A8BC',
    textMuted: '#73738A',
    border: '#2A2A38',
    tint: '#A78BFA',
    primary: '#A78BFA',
    onPrimary: '#FFFFFF',
    success: '#22C55E',
    info: '#60A5FA',
    danger: '#F87171',
    warning: '#FBBF24',
    icon: '#A8A8BC',
    tabIconDefault: '#73738A',
    tabIconSelected: '#A78BFA',
    overlay: 'rgba(0,0,0,0.6)',
  },
} as const;

export const Spacing = { xs: 4, sm: 8, md: 12, lg: 16, xl: 24, xxl: 32 } as const;

export const Radii = { sm: 8, md: 12, lg: 16, xl: 24, pill: 999 } as const;

export const Typography = {
  display: { fontSize: 34, lineHeight: 40, fontWeight: '800' },
  title: { fontSize: 28, lineHeight: 34, fontWeight: '700' },
  heading: { fontSize: 22, lineHeight: 28, fontWeight: '700' },
  subtitle: { fontSize: 18, lineHeight: 24, fontWeight: '600' },
  body: { fontSize: 16, lineHeight: 24, fontWeight: '400' },
  bodyMedium: { fontSize: 16, lineHeight: 24, fontWeight: '600' },
  caption: { fontSize: 13, lineHeight: 18, fontWeight: '400' },
  overline: { fontSize: 12, lineHeight: 16, fontWeight: '600', letterSpacing: 0.8, textTransform: 'uppercase' },
} as const;

/**
 * Gradient stop arrays for expo-linear-gradient.
 * `brand` is for the hero only — never overlay small text on the bright cyan stop.
 * `primary` backs primary buttons; keep white labels >=16px / weight >=600 (AA-large >=3:1).
 */
export const Gradients = {
  brand: ['#7C3AED', '#EC4899', '#22D3EE'],
  primary: ['#7C3AED', '#DB2777'],
} as const;

/** Subtle card elevation: iOS uses shadow*, Android uses elevation. */
export const Shadows = {
  card: {
    shadowColor: '#000',
    shadowOpacity: 0.12,
    shadowRadius: 12,
    shadowOffset: { width: 0, height: 4 },
    elevation: 3,
  },
} as const;

export const Fonts = Platform.select({
  ios: {
    /** iOS `UIFontDescriptorSystemDesignDefault` */
    sans: 'system-ui',
    /** iOS `UIFontDescriptorSystemDesignSerif` */
    serif: 'ui-serif',
    /** iOS `UIFontDescriptorSystemDesignRounded` */
    rounded: 'ui-rounded',
    /** iOS `UIFontDescriptorSystemDesignMonospaced` */
    mono: 'ui-monospace',
  },
  default: {
    sans: 'normal',
    serif: 'serif',
    rounded: 'normal',
    mono: 'monospace',
  },
  web: {
    sans: "system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif",
    serif: "Georgia, 'Times New Roman', serif",
    rounded: "'SF Pro Rounded', 'Hiragino Maru Gothic ProN', Meiryo, 'MS PGothic', sans-serif",
    mono: "SFMono-Regular, Menlo, Monaco, Consolas, 'Liberation Mono', 'Courier New', monospace",
  },
});
