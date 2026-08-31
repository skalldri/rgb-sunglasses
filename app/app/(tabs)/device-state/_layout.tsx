import { Colors } from '@/constants/theme';
import { useColorScheme } from '@/hooks/use-color-scheme';
import { Stack } from 'expo-router';

import { AudioTelemetryProvider } from '@/context/audio-telemetry-context';
import React from 'react';

export default function DeviceStateLayout() {
  const colorScheme = useColorScheme();
  const palette = colorScheme === 'dark' ? Colors.dark : Colors.light;

  // Both screens render their own in-body header (back button + title on the detail page), so
  // the native header is hidden here rather than driven via Stack.Screen options from inside each
  // screen component (which needs a real navigator context that isn't present in unit tests).
  return (
    /* ONE telemetry provider for the whole device-state stack, mounted here for exactly the
       reason app/CLAUDE.md gives for the MCUmgr client: a pushed screen does NOT unmount the
       one below it. The tuning screen and the wizard are both in this stack, so a
       per-screen provider meant two notification registrations on one characteristic
       (against Android's ~15-slot budget), two 30 s watchdogs fighting over tier and rate,
       and a blur/focus ordering race in which either side's "stop" write could land just
       after the other armed — dead meters in the wizard, or dead meters back on the tuning
       screen at exactly the moment the user is told to watch them. */
    <AudioTelemetryProvider>
    <Stack
      screenOptions={{
        headerShown: false,
        contentStyle: { backgroundColor: palette.background },
      }}>
      <Stack.Screen name="index" />
      <Stack.Screen name="battery" />
      <Stack.Screen name="capture" />
      <Stack.Screen name="audio" />
      <Stack.Screen name="audio-calibrate" />
      <Stack.Screen name="[serviceUuid]" />
    </Stack>
    </AudioTelemetryProvider>
  );
}
