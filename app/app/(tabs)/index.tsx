import { StyleSheet } from 'react-native';

import { ThemedText } from '@/components/themed-text';
import { ThemedView } from '@/components/themed-view';

export default function HomeScreen() {
  return (
    <ThemedView style={styles.container}>
      <ThemedView style={styles.content}>
        <ThemedText type="title" style={styles.title}>
          🕶️ RGB Sunglasses
        </ThemedText>
        <ThemedText style={styles.subtitle}>
          Control your RGB sunglasses via Bluetooth
        </ThemedText>
        <ThemedView style={styles.instructions}>
          <ThemedText style={styles.instructionText}>
            To get started, go to the{' '}
            <ThemedText type="defaultSemiBold">Bluetooth</ThemedText> tab to find
            and connect to your sunglasses.
          </ThemedText>
        </ThemedView>
      </ThemedView>
    </ThemedView>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    justifyContent: 'center',
    alignItems: 'center',
    padding: 24,
  },
  content: {
    alignItems: 'center',
    maxWidth: 320,
  },
  title: {
    textAlign: 'center',
    marginBottom: 8,
  },
  subtitle: {
    textAlign: 'center',
    opacity: 0.7,
    marginBottom: 32,
  },
  instructions: {
    backgroundColor: 'rgba(128, 128, 128, 0.1)',
    borderRadius: 12,
    padding: 16,
  },
  instructionText: {
    textAlign: 'center',
    lineHeight: 22,
  },
});
