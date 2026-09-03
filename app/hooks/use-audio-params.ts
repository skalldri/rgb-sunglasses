import { useCallback, useMemo } from "react";

import {
  UUID_AUDIO_CONFIG_SERVICE,
  UUID_AUDIO_PARAM_RANGES,
  UUID_AUDIO_TELEMETRY_SERVICE,
} from "@/constants/bluetooth";
import { useBluetooth } from "@/context/bluetooth-context";
import { useAudioParamWriter } from "@/hooks/use-audio-param-writer";
import { parseAudioParamRanges } from "@/services/audio-param-ranges";
import {
  AUDIO_PARAMS,
  AUDIO_PARAM_ORDER,
  encodeParam,
  resolveAudioParams,
  type AudioParamKey,
  type AudioParamSpec,
  type ResolvedAudioParam,
} from "@/services/audio-params";

/**
 * One place that turns the connected device into "the audio parameters, and how to write one".
 *
 * Both the tuning screen and the calibration wizard need exactly this, and both had their own
 * near-verbatim copy of it — which had ALREADY diverged: one filtered current values with
 * `typeof v === "number"` and the other with `v !== null`, and the wizard carried a third
 * representation (a `byKey` map with its own null semantics) alongside. Any change to how a
 * parameter is resolved or written had to be made twice, and the second site was the one that
 * would be missed.
 *
 * `valueOf` and `currentValues` are derived from a SINGLE map here, so they cannot disagree
 * about whether a parameter is present.
 */
export interface UseAudioParamsResult {
  /** Every parameter the device actually exposes, in firmware declaration order. */
  resolved: ResolvedAudioParam[];
  /** Current value per parameter, preferring a local override while a thumb owns it. */
  currentValues: Partial<Record<AudioParamKey, number>>;
  /** Current value for one parameter, or null when the device does not expose it. */
  valueOf: (key: AudioParamKey) => number | null;
  /**
   * The RESOLVED spec for one parameter — the app-side table merged with whatever ranges the
   * firmware published. Every render and every encode must go through this rather than
   * reaching into AUDIO_PARAMS directly, or the firmware's ranges have no observable effect:
   * the merged spec was previously consulted only for `.key`/`.uuid`/`.value`, so sliders
   * still rendered and clamped against stale app-side minimums.
   */
  specOf: (key: AudioParamKey) => AudioParamSpec;
  /** True while a write to that parameter is in flight. */
  busyOf: (key: AudioParamKey) => boolean;
  /** Write immediately, no throttling. Resolves false (never throws) on a rejected write. */
  writeParam: (key: AudioParamKey, value: number) => Promise<boolean>;
  /** The underlying writer, for slider drag/settle semantics. */
  writer: ReturnType<typeof useAudioParamWriter>;
  /**
   * The most recent failed write across the audio characteristics, or null.
   *
   * Both consumers surface it, so it is resolved here rather than duplicated per screen.
   */
  writeFailure: { label: string; reason: string } | null;
  /** True when the device exposes no audio configuration at all. */
  absent: boolean;
}

export function useAudioParams(): UseAudioParamsResult {
  const { selectedDevice, writeServiceCharacteristic } = useBluetooth();

  const serviceChars =
    selectedDevice?.characteristicsByService?.[UUID_AUDIO_CONFIG_SERVICE];

  const write = useCallback(
    (uuid: string, encoded: string) =>
      writeServiceCharacteristic(UUID_AUDIO_CONFIG_SERVICE, uuid, encoded),
    [writeServiceCharacteristic],
  );
  const writer = useAudioParamWriter(useMemo(() => ({ write }), [write]));

  /* Ranges, defaults and steps from the firmware's own table — the same table it clamps
   * against — so a slider cannot offer a value the device would reject and "reset to
   * defaults" restores what THIS image ships. Both beat_alpha (3.5 -> 0.3) and
   * noise_gate_rms (0.001 -> 0.0006) were retuned after app builds shipped, so an app-side
   * defaults table restores values that were measured to be wrong.
   *
   * Absent on older firmware, where parseAudioParamRanges returns null and the app-side table
   * stays authoritative — the seam resolveAudioParams was built with.
   *
   * Keyed on the characteristic's VALUE, not on selectedDevice: depending on the whole device
   * object re-parsed a ~413-byte blob and minted a new overrides identity on every unrelated
   * mutation of it, which then invalidated `resolved` and everything derived from it. */
  const rangesValue =
    selectedDevice?.characteristicsByService?.[UUID_AUDIO_TELEMETRY_SERVICE]?.[
      UUID_AUDIO_PARAM_RANGES
    ]?.value;
  const rangeOverrides = useMemo(
    () => parseAudioParamRanges(rangesValue) ?? undefined,
    [rangesValue],
  );

  const resolved = useMemo(
    () => resolveAudioParams(serviceChars ?? {}, rangeOverrides),
    [serviceChars, rangeOverrides],
  );

  const currentValues = useMemo(() => {
    const out: Partial<Record<AudioParamKey, number>> = {};
    resolved.forEach((r) => {
      const v = writer.displayValue(r.spec.uuid, r.value);
      if (v !== null) out[r.spec.key] = v;
    });
    return out;
  }, [resolved, writer]);

  const valueOf = useCallback(
    (key: AudioParamKey): number | null => currentValues[key] ?? null,
    [currentValues],
  );

  const specByKey = useMemo(() => {
    const map = {} as Partial<Record<AudioParamKey, AudioParamSpec>>;
    resolved.forEach((r) => {
      map[r.spec.key] = r.spec;
    });
    return map;
  }, [resolved]);

  /* Falls back to the static table for a parameter the device does not expose, so callers
   * that ask about one never get undefined — but for anything the device DOES expose, the
   * firmware's ranges win. */
  const specOf = useCallback(
    (key: AudioParamKey): AudioParamSpec => specByKey[key] ?? AUDIO_PARAMS[key],
    [specByKey],
  );

  const busyOf = useCallback(
    (key: AudioParamKey): boolean =>
      serviceChars?.[AUDIO_PARAMS[key].uuid]?.isUpdateInProgress ?? false,
    [serviceChars],
  );

  const writeParam = useCallback(
    (key: AudioParamKey, value: number) => {
      /* The RESOLVED spec, so encodeParam clamps against the firmware's range rather than the
       * app's copy of it. */
      const spec = specOf(key);
      return writer.writeNow(spec.uuid, value, (v) => encodeParam(spec, v));
    },
    [writer, specOf],
  );

  /**
   * The most recent failed write across the audio characteristics, or null.
   *
   * Read from the context's own lastWriteError rather than the writer's onError callback:
   * onError only fires when the write function THROWS, and writeServiceCharacteristic never
   * does — it catches every BLE error and returns false. Lives here rather than in a screen
   * because both consumers need it and the raw characteristics are already in scope.
   */
  const writeFailure = useMemo(() => {
    if (!serviceChars) return null;
    for (const key of AUDIO_PARAM_ORDER) {
      const spec = AUDIO_PARAMS[key];
      const reason = serviceChars[spec.uuid]?.lastWriteError;
      if (reason) return { label: spec.friendlyLabel, reason };
    }
    return null;
  }, [serviceChars]);

  return {
    resolved,
    currentValues,
    valueOf,
    specOf,
    busyOf,
    writeParam,
    writer,
    writeFailure,
    absent: !serviceChars || resolved.length === 0,
  };
}
