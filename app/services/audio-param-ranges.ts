import {
  AUDIO_PARAM_ORDER,
  type AudioParamKey,
  type AudioParamSpec,
} from "@/services/audio-params";
import { decodeBytesFromBase64 } from "@/services/ble-value-codec";

/**
 * Decoder for the firmware's bulk "Audio Param Ranges" blob (service 9, characteristic 2).
 *
 * The blob is generated at compile time from fw/src/sound/audio_param_table.h, which is the
 * same table the firmware clamps against — so once this is wired up, the app's sliders cannot
 * offer a value the device would reject, and "Reset to factory defaults" restores the values
 * THIS image actually ships rather than whatever the app was built believing.
 *
 * That last point is not hypothetical. Both `beat_alpha` (3.5 -> 0.3) and `noise_gate_rms`
 * (0.001 -> 0.0006) were retuned after app builds had already shipped, so an app-side defaults
 * table would confidently restore stale values that were measured to be wrong.
 *
 * Wire format (little-endian), mirroring audio_param_blob.h:
 *   [version: 1][entry_count: 1]
 *   per entry, in GATT declaration order:
 *     [type: 1]  0 = float32, 1 = uint32, 2 = enum
 *     [unit_len: 1][unit bytes]
 *     [enum_len: 1][enum labels, "\n"-separated]
 *     [default: f32][min: f32][max: f32][step: f32]
 */

export const AUDIO_PARAM_BLOB_VERSION = 1;


/**
 * Decode the blob into per-parameter overrides keyed by AudioParamKey.
 *
 * Returns null — never a partial result — for an unknown version, a truncated buffer, or an
 * entry count that disagrees with the app's own parameter list. A partial decode is the
 * dangerous outcome here: it would silently apply a few firmware ranges and leave the rest on
 * app-side guesses, producing a mixture nobody designed and nobody can reproduce.
 */
export function parseAudioParamRanges(
  value?: string | null,
): Partial<Record<AudioParamKey, Partial<AudioParamSpec>>> | null {
  const bytes = decodeBytesFromBase64(value);
  if (!bytes || bytes.length < 2) return null;
  if (bytes[0] !== AUDIO_PARAM_BLOB_VERSION) return null;

  const count = bytes[1];
  /* The blob is positional: entry N describes the Nth characteristic in GATT declaration
   * order. The firmware's table is APPEND-ONLY, so a count GREATER than this app knows about
   * is a newer firmware with extra tunables — and its first N entries still describe exactly
   * the parameters this app has, in the same order. Decoding that prefix is the same
   * positional trust the UUID mapping already relies on.
   *
   * Refusing on any mismatch recreated the exact staleness this blob exists to prevent: the
   * first time firmware grew a 15th parameter, every already-shipped app would silently fall
   * back to its own table for all 14 it *did* know — including retuned defaults like
   * beat_alpha 3.5 -> 0.3, which is the motivating example for the whole feature.
   *
   * A count SMALLER than expected is still fatal: this app would be reading entries that do
   * not exist, and there is no prefix to trust. */
  if (count < AUDIO_PARAM_ORDER.length) return null;

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const out: Partial<Record<AudioParamKey, Partial<AudioParamSpec>>> = {};
  let o = 2;

  const readStr = (): string | null => {
    if (o >= bytes.length) return null;
    const len = bytes[o];
    o += 1;
    if (o + len > bytes.length) return null;
    let s = "";
    for (let i = 0; i < len; i++) s += String.fromCharCode(bytes[o + i]);
    o += len;
    return s;
  };

  /* Only the prefix this app understands. Entries beyond it belong to parameters this build
   * has no UI for, and the trailing-bytes rule below covers them. */
  for (let i = 0; i < AUDIO_PARAM_ORDER.length; i++) {
    if (o >= bytes.length) return null;
    const type = bytes[o];
    o += 1;
    if (type > 2) return null;

    const unit = readStr();
    if (unit === null) return null;
    const enumBlob = readStr();
    if (enumBlob === null) return null;

    if (o + 16 > bytes.length) return null;
    const defaultValue = view.getFloat32(o, true);
    const min = view.getFloat32(o + 4, true);
    const max = view.getFloat32(o + 8, true);
    const step = view.getFloat32(o + 12, true);
    o += 16;

    if (![defaultValue, min, max, step].every(Number.isFinite)) return null;
    /* An inverted or empty range would make a slider unusable and a clamp nonsensical; that is
     * a firmware bug, and applying it would just move the symptom into the app. */
    if (!(max > min)) return null;

    const key = AUDIO_PARAM_ORDER[i];
    const override: Partial<AudioParamSpec> = { min, max, defaultValue };
    /* Step 0 means "no meaningful increment" — keep the app's own, which is chosen for the
     * slider's feel rather than for the firmware's clamp granularity. */
    if (step > 0) override.step = step;
    if (enumBlob.length > 0) override.enumLabels = enumBlob.split("\n");
    out[key] = override;
  }

  /* Bytes after the last entry are ignored, which costs nothing. Note this is NOT general
   * forward compatibility: adding a field to each ENTRY would desync this positional walk
   * partway through rather than leave slack at the end, so that change needs a version bump —
   * which this decoder rejects outright, by design. A short blob is always fatal. */
  return out;
}
