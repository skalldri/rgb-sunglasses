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
  BAND_RATIO_MAX,
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
import {
  decodeBytesFromBase64,
  encodeUint32ToBase64,
} from "@/services/ble-value-codec";

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

/* How long the stream survives with no consumer before it is torn down.
 *
 * Not a debounce for its own sake: expo-router fires the outgoing screen's blur BEFORE the
 * incoming screen's focus, so pushing Calibrate on top of Tuning drops the consumer count to
 * zero for one tick. Without this window that handoff would stop the stream and immediately
 * re-arm it — a control write, a CCCD teardown, a re-subscribe and a ring reset, at the exact
 * moment the wizard is telling the user to watch the meters. */
const STREAM_RELEASE_GRACE_MS = 750;
/** Re-arm at half the hold, per the firmware's documented watchdog contract. */
const REARM_MS = (REQUESTED_HOLD_S / 2) * 1000;
/** Summary recompute rate. Slow on purpose — this is the only path that renders. */
const SUMMARY_TICK_MS = 500;
/** One retry when the device is not yet in context at focus time. Mirrors ARM_RETRY_MS. */
const ARM_RETRY_MS = 1500;
/** Per-frame decay of the spectrum's normalisation reference (~2 s to halve at 8 Hz). */
const BUCKET_REF_DECAY = 0.96;
/** Starting reference, and what a re-arm resets to. */
const BUCKET_REF_SEED = 0.01;
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
  /**
   * Ask for a different tier/rate — used only by the calibration wizard's tap-along step,
   * which needs UNDECIMATED tier-2 frames.
   *
   * Both halves matter. The refractory is counted in analysis frames, so replaying a
   * decimated window would model a longer refractory than the device applies; and evaluating
   * a different alpha needs the raw mean/sigma that only tier 2 carries, because the resolved
   * threshold on the wire has the floor folded in and cannot be inverted back.
   *
   * Passing null restores the screen's default. The request is remembered, so the watchdog
   * re-arm keeps asking for the same thing rather than silently dropping back.
   */
  requestStream: (tier: TelemetryTier | null, rateHz?: number) => void;
  /**
   * Declare that this caller is displaying telemetry. Returns its release function.
   *
   * The stream is armed while at least one consumer is held and torn down (after a short
   * grace) when the last one releases. Prefer `useAudioTelemetryStream()`, which ties the
   * acquire/release to screen focus.
   */
  acquireStream: () => () => void;
};

const AudioTelemetryContext =
  React.createContext<AudioTelemetryContextValue | null>(null);

/* BAND_RATIO_MAX is imported from the telemetry service — see its comment there for why it
 * cannot live next to either consumer. */

/* Packs the control word; the little-endian u32 -> base64 encode itself is delegated, so the
 * byte-order convention lives in exactly one place for every characteristic on the device. */
function encodeControl(tier: number, rateHz: number, holdS: number): string {
  const word =
    ((tier & 0xff) | ((rateHz & 0xff) << 8) | ((holdS & 0xff) << 16)) >>> 0;
  return encodeUint32ToBase64(word);
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
  const bucketRefRef = React.useRef(BUCKET_REF_SEED);
  /* What we are currently asking the device for. Every control write reads THIS, never the
   * module constants — otherwise the 30 s watchdog re-arm silently reverts a wizard step that
   * raised the tier/rate, mid-recording. */
  const requestRef = React.useRef<{ tier: TelemetryTier; rateHz: number }>({
    tier: REQUESTED_TIER,
    rateHz: REQUESTED_RATE_HZ,
  });
  /* Assigned by arm() while armed, cleared on teardown. */
  const writeControlRef = React.useRef<
    ((tier: TelemetryTier, rateHz: number) => void) | null
  >(null);
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

  /* Consumer bookkeeping — see acquireStream() and the gate at the top of the focus effect. */
  const consumersRef = React.useRef(0);
  const releaseTimerRef = React.useRef<ReturnType<typeof setTimeout> | null>(null);
  const [streamWanted, setStreamWanted] = React.useState(false);

  const acquireStream = React.useCallback((): (() => void) => {
    consumersRef.current += 1;
    if (releaseTimerRef.current) {
      clearTimeout(releaseTimerRef.current);
      releaseTimerRef.current = null;
    }
    setStreamWanted(true);

    let released = false;
    return () => {
      /* Idempotent: React can invoke a focus effect's cleanup more than once across a
       * remount, and a double release would drive the count negative — after which the
       * stream could never be torn down again. */
      if (released) return;
      released = true;
      consumersRef.current = Math.max(0, consumersRef.current - 1);
      if (consumersRef.current > 0) return;
      if (releaseTimerRef.current) clearTimeout(releaseTimerRef.current);
      releaseTimerRef.current = setTimeout(() => {
        releaseTimerRef.current = null;
        if (consumersRef.current === 0) setStreamWanted(false);
      }, STREAM_RELEASE_GRACE_MS);
    };
  }, []);

  React.useEffect(
    () => () => {
      if (releaseTimerRef.current) clearTimeout(releaseTimerRef.current);
    },
    [],
  );

  useFocusEffect(
    React.useCallback(() => {
      /* DEMAND-DRIVEN, not focus-driven.
       *
       * This provider is mounted on the whole device-state stack (that layout's comment
       * explains why there must be exactly one), but "the provider is focused" is a different
       * question from "something on screen is displaying telemetry", and only the first was
       * ever gated on. Every screen in the stack — the Controls animation list, Battery,
       * Capture, the generic service page — therefore armed the stream just by being open.
       *
       * Measured on hardware with the phone sitting on the animation list: the board logged
       * `telemetry stream started: tier 3, 8 Hz, hold 60 s` (tier 3 is the full spectrogram,
       * the most expensive tier) and `conn param request: MEDIUM`, dragging the connection
       * interval from 11.25 ms to 45 ms — which slows every other GATT operation on that
       * screen — re-armed every 30 s so it never relaxed, with the device-side DSP
       * accumulation running the whole time. Nothing on that screen renders a meter.
       *
       * Only the tuning screen and the wizard do, and they now say so by acquiring. */
      if (!streamWanted) return;

      const generation = ++generationRef.current;
      let subscription: { remove: () => void } | null = null;
      let rearmTimer: ReturnType<typeof setInterval> | null = null;
      let summaryTimer: ReturnType<typeof setInterval> | null = null;
      let armTimer: ReturnType<typeof setInterval> | null = null;
      let controlChar: {
        writeWithResponse?: (v: string) => Promise<unknown>;
      } | null = null;

      const superseded = () => generationRef.current !== generation;

      /* Restore the default request when this focus session ends: the wizard raises the
       * tier/rate, and leaving requestRef raised means the next arm silently starts at the
       * wizard's burst settings. */
      const resetRequest = () => {
        requestRef.current = { tier: REQUESTED_TIER, rateHz: REQUESTED_RATE_HZ };
      };

      const stopRearm = () => {
        if (rearmTimer) {
          clearInterval(rearmTimer);
          rearmTimer = null;
        }
      };

      /* Re-open the arm retry after a recoverable failure. Idempotent: an already-running
       * retry is left alone rather than stacked, because arm() is also what clears it. */
      const scheduleArmRetry = () => {
        if (superseded() || armTimer) return;
        subscription = null;
        armTimer = setInterval(arm, ARM_RETRY_MS);
      };

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

      function arm() {
        if (superseded()) return;
        const device = deviceRef.current;
        const byService =
          device?.characteristicsByService?.[UUID_AUDIO_TELEMETRY_SERVICE];
        const streamInfo = byService?.[UUID_AUDIO_TELEMETRY];
        const controlInfo = byService?.[UUID_TELEMETRY_CONTROL];

        if (!byService) {
          /* No device, or discovery has not produced this service yet. NOT terminal — say
           * nothing and let the retry interval try again. */
          if (!device?.characteristicsByService) return;
          /* The device HAS finished discovering and this service is not on it: genuinely old
           * firmware. Now it is safe to latch. */
          setStatus("unsupported");
          if (armTimer) {
            clearInterval(armTimer);
            armTimer = null;
          }
          return;
        }
        if (!streamInfo || !controlInfo) {
          /* Service present but missing a characteristic — a firmware mismatch we cannot use.
           * Terminal for the same reason as above. */
          setStatus("unsupported");
          if (armTimer) {
            clearInterval(armTimer);
            armTimer = null;
          }
          return;
        }
        /* Armed successfully: stop retrying. */
        if (armTimer) {
          clearInterval(armTimer);
          armTimer = null;
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
                /* STOP RE-ARMING on any subscription end, expected or not.
                 *
                 * The re-arm interval kept firing every 30 s over a dead subscription,
                 * extending the firmware's watchdog hold so the device carried on encoding
                 * and notifying at 8-32 Hz into nothing — and the connection-parameter
                 * governor kept holding the faster interval, which is precisely the battery
                 * cost the stream-hold design exists to avoid. */
                stopRearm();

                /* remove() delivers OperationCancelled and a dropped link delivers a
                 * disconnect error — both are how a subscription normally ends. A cancel is
                 * our own teardown and needs nothing more. A disconnect can come back, so
                 * re-open the arm retry: without it the meters stayed dead after a mid-focus
                 * link drop even once the link returned, until the user blurred and
                 * refocused. */
                if (/cancel/i.test(text)) return;
                if (/disconnect/i.test(text)) {
                  setStatus("idle");
                  scheduleArmRetry();
                  return;
                }
                /* A hard error is NOT retried. Disconnects come back; an encryption or
                 * permission failure does not, and retrying it every 1.5 s would hammer the
                 * link for as long as the screen stays focused. Stopping here leaves a state
                 * the focus effect recovers from on the next blur/focus, which is the
                 * user-visible action that could plausibly change the outcome. */
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
        /* Reset the spectrum's normalisation reference alongside the ring. Left at a loud
         * session's level it decays at 0.96/frame, which at 8 Hz is ~14-20 s of near-invisible
         * bars in the next (quieter) session — the panel looks broken for the first quarter
         * minute after opening the screen, which is exactly when the user is deciding whether
         * to trust it. */
        bucketRefRef.current = BUCKET_REF_SEED;

        /* WIRE UP requestStream(). Without this assignment the whole wizard tap step was
         * inert: requestStream() reset the ring and sent nothing, so the device stayed at
         * tier 1 / 8 Hz, every recorded window measured ~125 ms spacing, and the spacing
         * guard in use-audio-calibration.ts refused the sensitivity fit on every run of
         * every device. The hook tests inject a fake requestStream, so nothing caught it. */
        writeControlRef.current = (tier, rateHz) => {
          if (superseded()) return;
          safeWrite(encodeControl(tier, rateHz, REQUESTED_HOLD_S), "request");
        };

        /* EVERY control write reads requestRef, never the module constants. The re-arm
         * interval is 30 s and the tap step is 30 s long, so a re-arm that encoded the
         * defaults would revert the stream to tier 3 / 8 Hz partway through the recording —
         * straddling a rate change, which is precisely what the window's spacing guard
         * refuses. The fit would then be discarded depending on where the tick happened to
         * land. */
        const armReq = requestRef.current;
        safeWrite(
          encodeControl(armReq.tier, armReq.rateHz, REQUESTED_HOLD_S),
          "arm",
        );

        /* The watchdog exists so a phone that backgrounds or walks away cannot leave the
         * device notifying into a void until the battery dies. Re-arming is this side of
         * that contract, and it is why the hold is short. */
        rearmTimer = setInterval(() => {
          if (superseded()) return;
          const req = requestRef.current;
          safeWrite(
            encodeControl(req.tier, req.rateHz, REQUESTED_HOLD_S),
            "re-arm",
          );
        }, REARM_MS);
      }

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

      /* Keep retrying while the device simply has not finished discovering.
       *
       * One 1.5 s retry was not enough: discovery is ~170 sequential GATT reads, comfortably
       * over that on a slow link, and `selectedDevice` is only populated once it completes.
       * Focusing this screen mid-discovery therefore latched `unsupported` permanently and
       * told the user their current firmware was too old, with no path back except blurring
       * and refocusing. arm() now distinguishes "no device yet" (retry) from "device present,
       * service absent" (genuinely unsupported), and only the latter is terminal. */
      if (deviceRef.current?.characteristicsByService) {
        arm();
      } else {
        armTimer = setInterval(arm, ARM_RETRY_MS);
      }

      return () => {
        /* Bump first: any callback still in flight from this pass becomes a no-op rather
         * than writing a stale frame into a ring the next pass has already reset. */
        generationRef.current++;
        if (armTimer) clearInterval(armTimer);
        if (rearmTimer) clearInterval(rearmTimer);
        if (summaryTimer) clearInterval(summaryTimer);

        /* Stop explicitly so the device drops the connection-parameter hold promptly. This
         * is politeness, not safety: unsubscribing below makes the firmware's next tick
         * self-terminate, and the watchdog catches the case where neither reaches it. */
        /* Clear before the stop write: a requestStream() racing teardown must not be able
         * to re-arm the device after we have told it to stop. */
        writeControlRef.current = null;
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
        resetRequest();
        summaryRef.current = EMPTY_SUMMARY;
        summaryListeners.current.forEach((l) => l());
        setStatus("idle");
      };
    }, [setStatus, streamWanted]),
  );

  const requestStream = React.useCallback((tier: TelemetryTier | null, rateHz?: number) => {
    const next = {
      tier: tier ?? REQUESTED_TIER,
      rateHz: rateHz ?? REQUESTED_RATE_HZ,
    };
    if (next.tier === requestRef.current.tier && next.rateHz === requestRef.current.rateHz) {
      return; // no edge, no write — the device treats a repeat as a hold extension
    }
    requestRef.current = next;
    /* Drop the history: a window spanning a rate change mixes two frame spacings, and the
     * replay counts refractory in frames. Better to start the recording clean. */
    resetTelemetryRing(ringRef.current);
    writeControlRef.current?.(next.tier, next.rateHz);
  }, []);

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
      requestStream,
      acquireStream,
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

/**
 * Declare that this screen is displaying telemetry, for as long as it is focused.
 *
 * A screen that renders any meter must call this. Nothing else starts the stream: the
 * provider spans the whole device-state stack, so without an explicit consumer the Controls
 * list, Battery and Capture screens would each arm a tier-3 8 Hz stream and hold the
 * connection interval down while showing nothing that uses it.
 *
 * Tied to FOCUS rather than mount, because a pushed screen does not unmount the one below it
 * — a mount-scoped acquire would keep the stream alive underneath whatever is on top.
 */
export function useAudioTelemetryStream(): void {
  const ctx = useAudioTelemetry();
  const acquire = ctx?.acquireStream;
  useFocusEffect(
    React.useCallback(() => {
      if (!acquire) return;
      return acquire();
    }, [acquire]),
  );
}

export { REQUESTED_RATE_HZ, REQUESTED_TIER, REQUESTED_HOLD_S, encodeControl };
