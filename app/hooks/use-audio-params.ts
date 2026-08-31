import { useCallback, useMemo } from "react";

import { UUID_AUDIO_CONFIG_SERVICE } from "@/constants/bluetooth";
import { useBluetooth } from "@/context/bluetooth-context";
import { useAudioParamWriter } from "@/hooks/use-audio-param-writer";
import {
  AUDIO_PARAMS,
  AUDIO_PARAM_ORDER,
  encodeParam,
  resolveAudioParams,
  type AudioParamKey,
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

  /* resolveAudioParams takes an `overrides` argument this hook does not yet supply — that is
   * the seam the firmware's published ranges plug into, and it lands with that firmware. Doing
   * it here rather than in a screen means it is wired once for every consumer. */
  const resolved = useMemo(
    () => resolveAudioParams(serviceChars ?? {}),
    [serviceChars],
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

  const busyOf = useCallback(
    (key: AudioParamKey): boolean =>
      serviceChars?.[AUDIO_PARAMS[key].uuid]?.isUpdateInProgress ?? false,
    [serviceChars],
  );

  const writeParam = useCallback(
    (key: AudioParamKey, value: number) => {
      const spec = AUDIO_PARAMS[key];
      return writer.writeNow(spec.uuid, value, (v) => encodeParam(spec, v));
    },
    [writer],
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
    busyOf,
    writeParam,
    writer,
    writeFailure,
    absent: !serviceChars || resolved.length === 0,
  };
}
