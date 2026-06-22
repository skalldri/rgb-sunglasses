import { Redirect } from 'expo-router';

// The old welcome screen is gone; the app opens on the Connect tab.
export default function Index() {
  return <Redirect href="/(tabs)/bluetooth" />;
}
