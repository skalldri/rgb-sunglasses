import { useFocusEffect } from "expo-router";
import React from "react";
import { useSharedValue, type SharedValue } from "react-native-reanimated";

import {
  UUID_AUDIO_TELEMETRY,
  UUID_AUDIO_TELEMETRY_SERVICE,
  UUID_TELEMETRY_CONTROL,
} from "@/constants/bluetooth";
import { useBluetooth } from "@/context/bluetooth-context";
import {
  AUDIO_NUM_BANDS,
  AUDIO_NUM_DISPLAY_BUCKETS,
  EMPTY_SUMMARY,
  SUMMARY_WINDOW_MS,
  TELEMETRY_TIER_OFF,
  TELEMETRY_TIER_SPECTRUM,
  createTelemetryRing,
  decodeBucketsInto,
  dequantiseLog,
  magnitudeToDb,
  pushTelemetryBytes,
  resetTelemetryRing,
  ringIndex,
  summarizeTelemetry,
  type TelemetryRing,
  type TelemetrySummary,
  type TelemetryTier,
} from "@/services/audio-telemetry";
import { decodeBytesFromBase64 } from "@/services/ble-value-codec";

/**
 * Live audio telemetry: subscription, control lifecycle, and the two render paths.
 *
 * THE RE-RENDER HAZARD IS THE WHOLE DESIGN. `useBluetooth()`'s context value changes
 * identity on every `selectedDevice` mutation, so sinking a 32 Hz stream into it would
 * re-render the entire app tree 32x/s and force every characteristic to be re-decoded each
 * time. That is why this deliberately does NOT use `useScopedCharacteristicMonitors` — that
 * hook's entire purpose is to sink values into the Bluetooth context, which is exactly the
 * thing that must not happen here.
 *
 * It does copy that hook's proven structure, because every piece of it was load-bearing:
 * refs-only effect deps (a context-derived dep is the documented ~11 reads/s feedback loop),
 * a generation counter for the blur -> focus race that rxandroidble does not serialize, the
 * `safeRead`-style optional-call-plus-try/catch because ble-plx throws SYNCHRONOUSLY after
 * teardown, the cancel/disconnect error filter, and the deferred re-arm for a device that
 * arrives after focus.
 *
 * Three paths out, in descending frequency:
 *
 *   1. Shared values, per frame, no React at all. Meters read these through
 *      useAnimatedStyle on the UI thread.
 *   2. The ring, per frame, refs only. The wizard replays it; nothing renders from it.
 *   3. A 2 Hz summary via useSyncExternalStore. Only the scoreboard and the verdict banner
 *      re-render, twice a second.
 *
 * The context value itself is memoised with EMPTY deps: it holds only refs, shared values
 * and stable callbacks, so it never changes identity and can never be the cause of a render.
 */

/** What we ask the firmware for. It clamps to what the link's MTU can carry and says so. */
const REQUESTED_TIER: TelemetryTier = TELEMETRY_TIER_SPECTRUM;
const REQUESTED_RATE_HZ = 8;
const REQUESTED_HOLD_S = 60;
/** Re-arm at half the hold, per the firmware's documented watchdog contract. */
const REARM_MS = (REQUESTED_HOLD_S / 2) * 1000;
/** Summary recompute rate. Slow on purpose — this is the only path that renders. */
const SUMMARY_TICK_MS = 500;
/** One retry when the device is not yet in context at focus time. Mirrors ARM_RETRY_MS. */
const ARM_RETRY_MS = 1500;
/** Per-frame decay of the spectrum's normalisation reference (~2 s to halve at 8 Hz). */
const BUCKET_REF_DECAY = 0.96;
/** Floor for that reference, so a silent room does not amplify quantisation noise to full. */
const BUCKET_REF_MIN = 1e-3;

export type TelemetryStatus =
  | "unsupported" // firmware without service 9
  | "idle" // supported, not started
  | "starting"
  | "streaming"
  | "error";

export type AudioTelemetryShared = {
  /** dBFS, already floored. Meters animate these on the UI thread. */
  rmsInputDb: SharedValue<number>;
  peakDb: SharedValue<number>;
  noiseFloorDb: SharedValue<number>;
  gainDb: SharedValue<number>;
  /** flux / threshold per band, clamped to 0..1.5. 1.0 is the fire line. */
  bandRatio: SharedValue<number>[];
  /** 20 display buckets, normalised 0..1 against the loudest bucket seen recently. */
  buckets: SharedValue<number>[];
  /** Monotonic counter, bumped once per frame carrying any beat. Drives the pulse. */
  beatTick: SharedValue<number>;
  /** Lowest band that fired on the most recent beat, or -1. */
  beatBand: SharedValue<number>;
  /** 1 while frames are arriving, 0 once the stream has gone stale. */
  liveness: SharedValue<number>;
};

export type AudioTelemetryContextValue = {
  ring: React.MutableRefObject<TelemetryRing>;
  shared: AudioTelemetryShared;
  subscribeSummary: (listener: () => void) => () => void;
  getSummarySnapshot: () => TelemetrySummary;
  getStatus: () => TelemetryStatus;
  subscribeStatus: (listener: () => void) => () => void;
};

const AudioTelemetryContext =
  React.createContext<AudioTelemetryContextValue | null>(null);

/** Ratio at which a band bar is drawn full. Above the fire line there is nothing more to say. */
const BAND_RATIO_MAX = 1.5;

function encodeControl(tier: number, rateHz: number, holdS: number): string {
  const word =
    ((tier & 0xff) | ((rateHz & 0xff) << 8) | ((holdS & 0xff) << 16)) >>> 0;
  const bytes = [
    word & 0xff,
    (word >> 8) & 0xff,
    (word >> 16) & 0xff,
    (word >> 24) & 0xff,
  ];
  return btoa(String.fromCharCode(...bytes));
}

export function AudioTelemetryProvider({
  children,
}: {
  children: React.ReactNode;
}) {
  const { selectedDevice } = useBluetooth();

  const ringRef = React.useRef<TelemetryRing>(createTelemetryRing());
  const deviceRef = React.useRef(selectedDevice);
  deviceRef.current = selectedDevice;
  const generationRef = React.useRef(0);
  /* Running reference for normalising the spectrum. Buckets are absolute magnitudes, so
   * without a reference the bars would sit invisibly low at normal listening levels and slam
   * to full on a loud track. Decays slowly so the display does not re-scale on every frame. */
  const bucketRefRef = React.useRef(0.01);
  /* Reused across frames so the notify path allocates nothing. */
  const bucketScratchRef = React.useRef<number[]>(
    new Array(AUDIO_NUM_DISPLAY_BUCKETS).fill(0),
  );

  const shared: AudioTelemetryShared = {
    rmsInputDb: useSharedValue(-100),
    peakDb: useSharedValue(-100),
    noiseFloorDb: useSharedValue(-100),
    gainDb: useSharedValue(0),
    /* eslint-disable react-hooks/rules-of-hooks -- fixed-length loops over wire constants;
       the count can only change with a wire-format version bump, never at runtime. */
    bandRatio: Array.from({ length: AUDIO_NUM_BANDS }, () => useSharedValue(0)),
    buckets: Array.from({ length: AUDIO_NUM_DISPLAY_BUCKETS }, () =>
      useSharedValue(0),
    ),
    /* eslint-enable react-hooks/rules-of-hooks */
    beatTick: useSharedValue(0),
    beatBand: useSharedValue(-1),
    liveness: useSharedValue(0),
  };
  const sharedRef = React.useRef(shared);
  sharedRef.current = shared;

  /* ── summary store (path 3) ── */
  const summaryRef = React.useRef<TelemetrySummary>(EMPTY_SUMMARY);
  const summaryListeners = React.useRef(new Set<() => void>());
  const subscribeSummary = React.useCallback((listener: () => void) => {
    summaryListeners.current.add(listener);
    return () => summaryListeners.current.delete(listener);
  }, []);
  const getSummarySnapshot = React.useCallback(() => summaryRef.current, []);

  /* ── status store ── */
  const statusRef = React.useRef<TelemetryStatus>("idle");
  const statusListeners = React.useRef(new Set<() => void>());
  const subscribeStatus = React.useCallback((listener: () => void) => {
    statusListeners.current.add(listener);
    return () => statusListeners.current.delete(listener);
  }, []);
  const getStatus = React.useCallback(() => statusRef.current, []);
  const setStatus = React.useCallback((next: TelemetryStatus) => {
    if (statusRef.current === next) return;
    statusRef.current = next;
    statusListeners.current.forEach((l) => l());
  }, []);

  useFocusEffect(
    React.useCallback(() => {
      const generation = ++generationRef.current;
      let subscription: { remove: () => void } | null = null;
      let rearmTimer: ReturnType<typeof setInterval> | null = null;
      let summaryTimer: ReturnType<typeof setInterval> | null = null;
      let armTimer: ReturnType<typeof setTimeout> | null = null;
      let controlChar: {
        writeWithResponse?: (v: string) => Promise<unknown>;
      } | null = null;

      const superseded = () => generationRef.current !== generation;

      /* Fire-and-forget write. ble-plx can throw SYNCHRONOUSLY once the link is gone, which
       * a bare .catch() would miss entirely — same rule as the scoped-read helper. */
      const safeWrite = (value: string, label: string) => {
        try {
          const pending = controlChar?.writeWithResponse?.(value);
          if (!pending) return;
          pending.catch((err: unknown) => {
            if (superseded()) return;
            console.log(`Telemetry control write failed (${label}):`, err);
            setStatus("error");
          });
        } catch (err) {
          console.log(
            `Telemetry control write could not start (${label}):`,
            err,
          );
        }
      };

      const onFrame = (bytes: Uint8Array | null) => {
        if (superseded() || !bytes) return;
        const ring = ringRef.current;
        if (!pushTelemetryBytes(ring, bytes, Date.now())) return;

        const s = sharedRef.current;
        const i = ringIndex(ring, 0);
        const base = i * AUDIO_NUM_BANDS;

        s.rmsInputDb.value = magnitudeToDb(dequantiseLog(ring.rmsIn[i]));
        s.peakDb.value = magnitudeToDb(dequantiseLog(ring.peak[i]));
        s.noiseFloorDb.value = magnitudeToDb(dequantiseLog(ring.noise[i]));
        s.gainDb.value = ring.gain[i] * 0.5;
        s.liveness.value = 1;

        for (let b = 0; b < AUDIO_NUM_BANDS; b++) {
          const flux = dequantiseLog(ring.flux[base + b]);
          const threshold = dequantiseLog(ring.threshold[base + b]);
          /* A zero threshold means the detector had no fire line to apply, not "everything
           * fires" — draw the bar empty rather than pinned. */
          const ratio =
            threshold > 0 ? Math.min(flux / threshold, BAND_RATIO_MAX) : 0;
          s.bandRatio[b].value = ratio;
        }

        /* Buckets are display-only, so they go straight to shared values and never enter the
         * ring — the wizard replays flux/mean/sigma, not the spectrum, and ringing 20 more
         * bytes per frame would cost 10 KB to store something nothing reads back. */
        const frameMax = decodeBucketsInto(bytes, bucketScratchRef.current);
        if (frameMax >= 0) {
          const ref = Math.max(
            frameMax,
            bucketRefRef.current * BUCKET_REF_DECAY,
            BUCKET_REF_MIN,
          );
          bucketRefRef.current = ref;
          const scratch = bucketScratchRef.current;
          for (let k = 0; k < AUDIO_NUM_DISPLAY_BUCKETS; k++) {
            s.buckets[k].value = scratch[k] / ref;
          }
        }

        const mask = (ring.flags[i] >> 4) & 0x0f;
        if (mask !== 0) {
          for (let b = 0; b < AUDIO_NUM_BANDS; b++) {
            if (mask & (1 << b)) {
              s.beatBand.value = b;
              break;
            }
          }
          s.beatTick.value = s.beatTick.value + 1;
        }
      };

      const arm = () => {
        if (superseded()) return;
        const device = deviceRef.current;
        const byService =
          device?.characteristicsByService?.[UUID_AUDIO_TELEMETRY_SERVICE];
        const streamInfo = byService?.[UUID_AUDIO_TELEMETRY];
        const controlInfo = byService?.[UUID_TELEMETRY_CONTROL];

        if (!streamInfo || !controlInfo) {
          /* Firmware without service 9. The screen still tunes; it just cannot show the
           * room. Not an error — the app ships ahead of firmware by design. */
          setStatus("unsupported");
          return;
        }
        controlChar = controlInfo.characteristic as typeof controlChar;
        setStatus("starting");

        /* SUBSCRIBE BEFORE ARMING. The firmware rejects a non-zero tier written by a client
         * that is not yet subscribed with -EACCES, deliberately: ATT serializes on one
         * bearer, so a control-first write cannot get its CCCD write in before the first
         * tick, and that tick would kill an ACKed stream with nothing left to re-arm it. */
        try {
          subscription = streamInfo.characteristic.monitor(
            (
              error: { message?: string } | null,
              characteristic: { value?: string | null } | null,
            ) => {
              /* FIRST, before anything observable. rxandroidble tears a subscription down
               * fire-and-forget, so a callback from a superseded pass is routine — and until
               * this guard was hoisted here, such a callback flipped the status back to
               * 'streaming' after teardown even though its frame was correctly discarded.
               * That left the pill reading LIVE over a dead stream, which is the one thing
               * these meters must never do. */
              if (superseded()) return;
              if (error) {
                const text = error?.message || String(error);
                /* remove() delivers OperationCancelled and a dropped link delivers a
                 * disconnect error — both are how a subscription normally ends. */
                if (/cancel/i.test(text) || /disconnect/i.test(text)) return;
                console.error("Telemetry notification error:", error);
                setStatus("error");
                return;
              }
              if (!characteristic?.value) return;
              if (statusRef.current !== "streaming") setStatus("streaming");
              onFrame(decodeBytesFromBase64(characteristic.value));
            },
          );
        } catch (err) {
          console.log("Telemetry monitor could not start:", err);
          setStatus("error");
          return;
        }

        resetTelemetryRing(ringRef.current);
        safeWrite(
          encodeControl(REQUESTED_TIER, REQUESTED_RATE_HZ, REQUESTED_HOLD_S),
          "arm",
        );

        /* The watchdog exists so a phone that backgrounds or walks away cannot leave the
         * device notifying into a void until the battery dies. Re-arming is this side of
         * that contract, and it is why the hold is short. */
        rearmTimer = setInterval(() => {
          if (superseded()) return;
          safeWrite(
            encodeControl(REQUESTED_TIER, REQUESTED_RATE_HZ, REQUESTED_HOLD_S),
            "re-arm",
          );
        }, REARM_MS);
      };

      summaryTimer = setInterval(() => {
        if (superseded()) return;
        const next = summarizeTelemetry(
          ringRef.current,
          Date.now(),
          SUMMARY_WINDOW_MS,
        );
        summaryRef.current = next;
        sharedRef.current.liveness.value = next.live ? 1 : 0;
        summaryListeners.current.forEach((l) => l());
      }, SUMMARY_TICK_MS);

      if (deviceRef.current?.characteristicsByService) {
        arm();
      } else {
        armTimer = setTimeout(arm, ARM_RETRY_MS);
      }

      return () => {
        /* Bump first: any callback still in flight from this pass becomes a no-op rather
         * than writing a stale frame into a ring the next pass has already reset. */
        generationRef.current++;
        if (armTimer) clearTimeout(armTimer);
        if (rearmTimer) clearInterval(rearmTimer);
        if (summaryTimer) clearInterval(summaryTimer);

        /* Stop explicitly so the device drops the connection-parameter hold promptly. This
         * is politeness, not safety: unsubscribing below makes the firmware's next tick
         * self-terminate, and the watchdog catches the case where neither reaches it. */
        safeWrite(encodeControl(TELEMETRY_TIER_OFF, 0, 0), "stop");
        try {
          subscription?.remove();
        } catch (err) {
          console.log("Telemetry monitor removal failed:", err);
        }
        /* Notify, do not just assign. A consumer can remain mounted across a blur (a pushed
         * screen does not unmount the one below it), and a silently-mutated ref leaves it
         * rendering the last status and the last numbers it was told about — stale meters
         * presented as current, which is exactly what the freeze-with-NO-SIGNAL behaviour
         * exists to prevent. */
        summaryRef.current = EMPTY_SUMMARY;
        summaryListeners.current.forEach((l) => l());
        setStatus("idle");
      };
    }, [setStatus]),
  );

  /* EMPTY deps, deliberately: every member is a ref, a shared value or a stable callback, so
   * this object must never change identity. If it did, it would re-render every consumer of
   * the telemetry context on whatever caused the change — which is the exact failure mode
   * this whole file exists to avoid. */
  const value = React.useMemo<AudioTelemetryContextValue>(
    () => ({
      ring: ringRef,
      shared: sharedRef.current,
      subscribeSummary,
      getSummarySnapshot,
      getStatus,
      subscribeStatus,
    }),
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [],
  );

  return (
    <AudioTelemetryContext.Provider value={value}>
      {children}
    </AudioTelemetryContext.Provider>
  );
}

export function useAudioTelemetry(): AudioTelemetryContextValue | null {
  return React.useContext(AudioTelemetryContext);
}

/** Re-renders at most twice a second, and only where it is called. */
export function useAudioTelemetrySummary(): TelemetrySummary {
  const ctx = useAudioTelemetry();
  const subscribe = ctx?.subscribeSummary ?? (() => () => {});
  const snapshot = ctx?.getSummarySnapshot ?? (() => EMPTY_SUMMARY);
  return React.useSyncExternalStore(subscribe, snapshot, snapshot);
}

export function useAudioTelemetryStatus(): TelemetryStatus {
  const ctx = useAudioTelemetry();
  const subscribe = ctx?.subscribeStatus ?? (() => () => {});
  const snapshot = ctx?.getStatus ?? ((): TelemetryStatus => "unsupported");
  return React.useSyncExternalStore(subscribe, snapshot, snapshot);
}

export { REQUESTED_RATE_HZ, REQUESTED_TIER, REQUESTED_HOLD_S, encodeControl };
