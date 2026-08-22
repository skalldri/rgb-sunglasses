/** Moved to core/wav.ts (ported Buffer → DataView, byte-exact fast path
 * preserved) so the browser scenario player decodes captured WAVs with the
 * SAME decoder as the CLI — browser-native decodeAudioData would resample
 * differently per browser and break replay determinism. Re-exported here so
 * existing node/ imports keep working; Buffer is a Uint8Array subclass, so
 * callers pass fs.readFileSync() results unchanged. */
export { decodeWavTo16kMono } from "../core/wav";
