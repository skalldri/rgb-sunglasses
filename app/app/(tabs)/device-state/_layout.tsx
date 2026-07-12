import { Colors } from '@/constants/theme';
import { useColorScheme } from '@/hooks/use-color-scheme';
import { Stack } from 'expo-router';
import React from 'react';

export default function DeviceStateLayout() {
  const colorScheme = useColorScheme();
  const palette = colorScheme === 'dark' ? Colors.dark : Colors.light;

  // Both screens render their own in-body header (back button + title on the detail page), so
  // the native header is hidden here rather than driven via Stack.Screen options from inside each
  // screen component (which needs a real navigator context that isn't present in unit tests).
  return (
    <Stack
      screenOptions={{
        headerShown: false,
        contentStyle: { backgroundColor: palette.background },
      }}>
      <Stack.Screen name="index" />
      <Stack.Screen name="battery" />
      <Stack.Screen name="[serviceUuid]" />
    </Stack>
  );
}
