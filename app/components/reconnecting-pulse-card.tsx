import { ReactNode, useEffect, useRef, useState } from 'react';
import { Animated, Easing, StyleSheet, View } from 'react-native';
import { LinearGradient } from 'expo-linear-gradient';

import { Radii, Shadows, Spacing } from '@/constants/theme';
import { useThemeColors } from '@/hooks/use-theme-color';

interface Props {
  children: ReactNode;
}

/**
 * Card-alike wrapper for the auto-reconnecting device row: a light band sweeps
 * across it on a loop and the border pulses, so "reconnecting" reads as visibly
 * distinct from the static "connected" highlight. Built on the plain `Animated`
 * API (the one animation convention already used in this codebase, see
 * hooks/use-characteristic-editor.tsx) rather than reanimated/react-native-svg,
 * since neither is otherwise used here and svg isn't even installed.
 */
export function ReconnectingPulseCard({ children }: Props) {
  const c = useThemeColors();
  const [width, setWidth] = useState(0);
  const sweep = useRef(new Animated.Value(0)).current;
  const pulse = useRef(new Animated.Value(0)).current;

  useEffect(() => {
    const sweepLoop = Animated.loop(
      Animated.timing(sweep, {
        toValue: 1,
        duration: 1600,
        easing: Easing.inOut(Easing.ease),
        useNativeDriver: true,
      })
    );
    // Color interpolation can't run on the native driver, matching the
    // useNativeDriver: false precedent for color fades elsewhere in the app.
    const pulseLoop = Animated.loop(
      Animated.sequence([
        Animated.timing(pulse, { toValue: 1, duration: 700, easing: Easing.inOut(Easing.ease), useNativeDriver: false }),
        Animated.timing(pulse, { toValue: 0, duration: 700, easing: Easing.inOut(Easing.ease), useNativeDriver: false }),
      ])
    );
    sweepLoop.start();
    pulseLoop.start();
    return () => {
      sweepLoop.stop();
      pulseLoop.stop();
    };
  }, [sweep, pulse]);

  const bandWidth = Math.max(width * 0.6, 80);
  const translateX = sweep.interpolate({
    inputRange: [0, 1],
    outputRange: [-bandWidth, width],
  });
  const borderColor = pulse.interpolate({
    inputRange: [0, 1],
    outputRange: [c.primary + '40', c.primary],
  });

  return (
    <Animated.View
      testID="reconnecting-pulse-card"
      onLayout={e => setWidth(e.nativeEvent.layout.width)}
      style={[styles.card, { backgroundColor: c.surface, borderColor }]}
    >
      <Animated.View
        pointerEvents="none"
        style={[styles.sweep, { width: bandWidth, transform: [{ translateX }] }]}
      >
        <LinearGradient
          colors={['transparent', c.primary + '30', 'transparent']}
          start={{ x: 0, y: 0 }}
          end={{ x: 1, y: 0 }}
          style={StyleSheet.absoluteFill}
        />
      </Animated.View>
      <View style={styles.content}>{children}</View>
    </Animated.View>
  );
}

const styles = StyleSheet.create({
  card: {
    borderRadius: Radii.lg,
    borderWidth: 2,
    overflow: 'hidden',
    ...Shadows.card,
  },
  sweep: {
    position: 'absolute',
    top: 0,
    bottom: 0,
  },
  content: {
    padding: Spacing.lg,
  },
});
