import { AUDIO_NUM_BANDS } from "@/services/audio-telemetry";
import { type CalibrationWindow } from "@/services/audio-calibration";

/**
 * Accumulating pools of recorded telemetry, gathered opportunistically.
 *
 * WHY THIS EXISTS. The wizard used to record three fixed windows on a countdown — 8 s of room,
 * 15 s of music, 30 s of tapping, in that order, starting when the app said so. That is
 * unworkable anywhere it actually matters: at a venue the music starts and stops on the band's
 * direction, not the app's, and a quiet moment lasts as long as it lasts. A timed step that
 * demands silence on cue produces a confident fit of whatever happened to be playing.
 *
 * So collection is opportunistic instead. Each pool accumulates CHUNKS — one per press of a
 * collector's toggle — and a pool is ready when it holds enough frames, however many sittings
 * that took. Nothing expires, nothing is ordered, and a chunk ruined halfway through (the band
 * came back early) costs that chunk and nothing else.
 *
 * WHY POOLS COPY RATHER THAN REMEMBER A TIME RANGE. The telemetry ring is circular and holds
 * RING_FRAMES entries — about 32 s at the tap collector's 32 Hz. The old design could point at
 * a start timestamp because its longest window was 30 s. Accumulating across a soundcheck
 * cannot: the frames would be overwritten long before the fit ran. Pools therefore drain the
 * ring into their own arrays as they go.
 *
 * CONCATENATION IS SOUND FOR ALL THREE CONSUMERS, which is what makes this cheap:
 *  - analyzeRoom / analyzeMusic take percentiles over rmsInput, which are order-independent and
 *    indifferent to gaps between chunks.
 *  - replayBeats walks frames in order and stamps beats from timeMs, so beat times stay
 *    absolute across a gap; matchTaps then compares absolute times, and a tap recorded in one
 *    chunk simply cannot match a beat in another. That is correct behaviour, not a limitation.
 */

/**
 * Per-chunk provenance.
 *
 * These are kept per chunk rather than only as pool-wide aggregates because the aggregates are
 * NOT REVERSIBLE, and "discard the last chunk" has to reverse them. `hasStats` is an AND, which
 * cannot be un-ANDed; `medianStepMs` is a max, which may have belonged to the chunk being
 * removed. An earlier attempt at this recomputed by resetting hasStats to true, which would
 * silently rehabilitate a pool whose FIRST chunk was decimated — the sweep would then fit a
 * refractory in frames against a window that never had the resolution to support one.
 */
type ChunkMeta = {
  frames: number;
  /** Wall-clock span of the chunk. */
  ms: number;
  hasStats: boolean;
  medianStepMs: number;
};

export type CalibrationPool = CalibrationWindow & {
  /**
   * Worst (largest) per-chunk median frame spacing. MAX, not a pool-wide median: diffs taken
   * across a chunk boundary are gap-sized and are not a spacing at all, and the consumer is a
   * refusal, so the conservative summary is the correct one.
   */
  medianStepMs: number;
  chunks: ChunkMeta[];
};

/** A drained slice of the ring, exactly as extractCalibrationWindow returns it. */
export type PoolChunk = CalibrationWindow & { medianStepMs: number };

export function emptyPool(): CalibrationPool {
  return {
    frames: 0,
    timeMs: [],
    rmsInput: [],
    clipped: [],
    beat: [],
    flux: [],
    mean: [],
    sigma: [],
    thresholdMode: 0,
    /* An empty pool has nothing decimated in it. Seeding `false` would make every pool refuse
     * the sweep until its first append, which reads as "this link cannot do it" rather than
     * "nothing collected yet" — a distinction the whole collector UI rests on. */
    hasStats: true,
    medianStepMs: 0,
    chunks: [],
  };
}

/** Total seconds of audio gathered, across every chunk. */
export function poolSeconds(pool: CalibrationPool): number {
  let ms = 0;
  for (const c of pool.chunks) ms += c.ms;
  return ms / 1000;
}

/** Recompute the pool-wide aggregates from surviving chunk metadata. */
function aggregate(chunks: ChunkMeta[]): {
  hasStats: boolean;
  medianStepMs: number;
} {
  let hasStats = true;
  let worst = 0;
  for (const c of chunks) {
    hasStats = hasStats && c.hasStats;
    worst = Math.max(worst, c.medianStepMs);
  }
  return { hasStats, medianStepMs: worst };
}

/**
 * Append one drained chunk. Returns a NEW pool; never mutates.
 *
 * A zero-frame chunk is dropped rather than recorded, so a toggle pressed and released before
 * any frame arrived does not leave an empty chunk for "discard last" to waste itself on.
 */
export function appendChunk(
  pool: CalibrationPool,
  chunk: PoolChunk,
): CalibrationPool {
  if (chunk.frames <= 0) return pool;

  const meta: ChunkMeta = {
    frames: chunk.frames,
    ms:
      chunk.timeMs.length > 1
        ? chunk.timeMs[chunk.timeMs.length - 1] - chunk.timeMs[0]
        : 0,
    hasStats: chunk.hasStats,
    medianStepMs: chunk.medianStepMs,
  };
  const chunks = [...pool.chunks, meta];
  const agg = aggregate(chunks);

  return {
    frames: pool.frames + chunk.frames,
    timeMs: [...pool.timeMs, ...chunk.timeMs],
    rmsInput: [...pool.rmsInput, ...chunk.rmsInput],
    clipped: [...pool.clipped, ...chunk.clipped],
    beat: [...pool.beat, ...chunk.beat],
    flux: [...pool.flux, ...chunk.flux],
    mean: [...pool.mean, ...chunk.mean],
    sigma: [...pool.sigma, ...chunk.sigma],
    /* The newest chunk wins: the threshold shape is a device setting the user can change
     * mid-session, and the fit must describe the device as it is now. */
    thresholdMode: chunk.thresholdMode,
    hasStats: agg.hasStats,
    medianStepMs: agg.medianStepMs,
    chunks,
  };
}

/** Drop the most recent chunk — the "that one was ruined" escape hatch. */
export function dropLastChunk(pool: CalibrationPool): CalibrationPool {
  const n = pool.chunks.length;
  if (n === 0) return pool;

  const keptFrames = pool.frames - pool.chunks[n - 1].frames;
  if (keptFrames <= 0) return emptyPool();

  const chunks = pool.chunks.slice(0, n - 1);
  const agg = aggregate(chunks);
  const bandEnd = keptFrames * AUDIO_NUM_BANDS;

  return {
    frames: keptFrames,
    timeMs: pool.timeMs.slice(0, keptFrames),
    rmsInput: pool.rmsInput.slice(0, keptFrames),
    clipped: pool.clipped.slice(0, keptFrames),
    beat: pool.beat.slice(0, keptFrames),
    flux: pool.flux.slice(0, bandEnd),
    mean: pool.mean.slice(0, bandEnd),
    sigma: pool.sigma.slice(0, bandEnd),
    thresholdMode: pool.thresholdMode,
    hasStats: agg.hasStats,
    medianStepMs: agg.medianStepMs,
    chunks,
  };
}

/** Readiness of one pool against its minimum frame count. */
export type PoolReadiness = {
  frames: number;
  seconds: number;
  needFrames: number;
  ready: boolean;
  chunks: number;
};

export function readiness(
  pool: CalibrationPool,
  needFrames: number,
): PoolReadiness {
  return {
    frames: pool.frames,
    seconds: poolSeconds(pool),
    needFrames,
    ready: pool.frames >= needFrames,
    chunks: pool.chunks.length,
  };
}
