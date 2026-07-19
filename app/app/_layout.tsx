import { DarkTheme, DefaultTheme, ThemeProvider, type Theme } from '@react-navigation/native';
import { Stack } from 'expo-router';
import { StatusBar } from 'expo-status-bar';
import 'react-native-reanimated';

import { Colors } from '@/constants/theme';
import { BluetoothProvider } from '@/context/bluetooth-context';
import { useBleAppStateVerify } from '@/hooks/use-ble-app-state';
import { useBleRestorationAdopt } from '@/hooks/use-ble-restoration';
import { useColorScheme } from '@/hooks/use-color-scheme';

export const unstable_settings = {
  anchor: '(tabs)',
};

const navLight: Theme = {
  ...DefaultTheme,
  colors: {
    ...DefaultTheme.colors,
    background: Colors.light.background,
    card: Colors.light.surface,
    text: Colors.light.textPrimary,
    primary: Colors.light.primary,
    border: Colors.light.border,
  },
};

const navDark: Theme = {
  ...DarkTheme,
  colors: {
    ...DarkTheme.colors,
    background: Colors.dark.background,
    card: Colors.dark.surface,
    text: Colors.dark.textPrimary,
    primary: Colors.dark.primary,
    border: Colors.dark.border,
  },
};

// Must be its own component (not a hook call in RootLayout): useBleAppStateVerify
// reads the Bluetooth context, so it has to render INSIDE BluetoothProvider.
function BleAppStateVerifier() {
  useBleAppStateVerify();
  return null;
}

// Same INSIDE-BluetoothProvider constraint as above: the iOS state-restoration
// adopter (issue #190) drives the reconnect machinery through the context.
function BleRestorationAdopter() {
  useBleRestorationAdopt();
  return null;
}

export default function RootLayout() {
  const colorScheme = useColorScheme();
  const palette = colorScheme === 'dark' ? Colors.dark : Colors.light;

  return (
    <BluetoothProvider>
      <BleAppStateVerifier />
      <BleRestorationAdopter />
      <ThemeProvider value={colorScheme === 'dark' ? navDark : navLight}>
        <Stack
          screenOptions={{
            headerStyle: { backgroundColor: palette.surface },
            headerTintColor: palette.textPrimary,
            contentStyle: { backgroundColor: palette.background },
          }}>
          <Stack.Screen name="(tabs)" options={{ headerShown: false }} />
          <Stack.Screen name="color-picker-modal" options={{ presentation: 'modal', title: 'Color Picker' }} />
          <Stack.Screen name="firmware-update-modal" options={{ presentation: 'modal', title: 'Firmware Update' }} />
          <Stack.Screen name="app-update-modal" options={{ presentation: 'modal', title: 'App Update' }} />
        </Stack>
        <StatusBar style="auto" />
      </ThemeProvider>
    </BluetoothProvider>
  );
}
