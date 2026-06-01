import { DarkTheme, DefaultTheme, ThemeProvider } from '@react-navigation/native';
import { Stack } from 'expo-router';
import { StatusBar } from 'expo-status-bar';
import 'react-native-reanimated';

import { BluetoothProvider } from '@/context/bluetooth-context';
import { useColorScheme } from '@/hooks/use-color-scheme';

export const unstable_settings = {
  anchor: '(tabs)',
};

export default function RootLayout() {
  const colorScheme = useColorScheme();

  return (
    <BluetoothProvider>
      <ThemeProvider value={colorScheme === 'dark' ? DarkTheme : DefaultTheme}>
        <Stack>
          <Stack.Screen name="(tabs)" options={{ headerShown: false }} />
          <Stack.Screen name="color-picker-modal" options={{ presentation: 'modal', title: 'Color Picker' }} />
          <Stack.Screen name="firmware-update-modal" options={{ presentation: 'modal', title: 'Firmware Update' }} />
        </Stack>
        <StatusBar style="auto" />
      </ThemeProvider>
    </BluetoothProvider>
  );
}
