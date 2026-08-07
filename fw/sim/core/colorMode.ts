/**
 * TypeScript port of the firmware's ColorModeSource
 * (fw/src/animations/color_mode_source.{h,cpp}, issue #259).
 *
 * COLOR parameter values carry a mode byte in bits 24-31 and (for non-Static
 * modes) a speed byte in bits 16-23. The HOST resolves this into the
 * effective per-tick 0x00RRGGBB before writing rgbx_inputs.params[p]
 * (extension_host.cpp:1177-1181); extensions never see the mode byte. This
 * port must stay in semantic lockstep with the C++ — the unit tests replay
 * known vectors derived from the C++ logic.
 */

export enum ColorMode {
  Static = 0x00,
  SpectrumSweep = 0x01,
  RandomOnBeat = 0x02,
  RandomOnActivate = 0x03,
  RandomTimerFade = 0x04,
}

const HUE_SPAN = 1536;
/** Full hue span in the Q16 fixed-point phase accumulator. */
const HUE_SPAN_Q16 = HUE_SPAN * 65536;

/** SpectrumSweep full-cycle period: speed 255 -> 2 s, speed 0 -> ~60.1 s. */
export function sweepPeriodMs(speed: number): number {
  return 2000 + (255 - speed) * 228;
}

/** RandomTimerFade pick interval: speed 255 -> 1 s, speed 0 -> ~30.1 s. */
export function timerIntervalMs(speed: number): number {
  return 1000 + (255 - speed) * 114;
}

/**
 * Integer 6-sector hue wheel at full saturation/value (anim_color_from_hue).
 * Every output has at least one channel at 255 so the color survives the
 * global-brightness scaling. Returns 0x00RRGGBB.
 */
export function animColorFromHue(hue1536: number): number {
  const h = hue1536 % HUE_SPAN;
  const sector = h >> 8;
  const ramp = h & 0xff;
  let r = 0;
  let g = 0;
  let b = 0;
  switch (sector) {
    case 0: r = 255; g = ramp; b = 0; break; // R -> Y
    case 1: r = 255 - ramp; g = 255; b = 0; break; // Y -> G
    case 2: r = 0; g = 255; b = ramp; break; // G -> C
    case 3: r = 0; g = 255 - ramp; b = 255; break; // C -> B
    case 4: r = ramp; g = 0; b = 255; break; // B -> M
    default: r = 255; g = 0; b = 255 - ramp; break; // M -> R
  }
  return (r << 16) | (g << 8) | b;
}

/** Hue-wheel lerp along the shorter arc, t256 in [0, 256] (hue_lerp). */
export function hueLerp(from: number, to: number, t256: number): number {
  let delta = to - from;
  if (delta > 768) {
    delta -= HUE_SPAN;
  } else if (delta < -768) {
    delta += HUE_SPAN;
  }
  const h = from + Math.trunc((delta * t256) / 256);
  return ((h % HUE_SPAN) + HUE_SPAN) % HUE_SPAN;
}

/** Beat feed for RandomOnBeat: true iff >= 1 beat since the previous call. */
export type ConsumeBeatFn = () => boolean;

/**
 * One resolver instance per COLOR param index (the firmware keeps a
 * ColorModeSource per param slot in sParamColorResolvers).
 */
export class ColorModeResolver {
  private resetPending = false;
  private stateValid = false;
  private lastMode = 0;
  private lastNowMs = 0;
  private huePhase16 = 0;
  private currentHue = 0;
  private prevHue = 0;
  private targetHue = 0;
  private segmentStartMs = 0;

  constructor(
    private readonly rng: () => number,
    private readonly now: () => number,
    private beatSource: ConsumeBeatFn | null = null,
  ) {}

  setBeatSource(src: ConsumeBeatFn | null): void {
    this.beatSource = src;
  }

  /** Arm a state reset (new random color, restarted phase) for the next
   * resolve() — the firmware's notifyActivated(). */
  notifyActivated(): void {
    this.resetPending = true;
  }

  /** New hue always >= 60 degrees (256/1536) from base (rollHueFrom). */
  private rollHueFrom(base: number): number {
    return (base + 256 + (this.rng() % 1024)) % HUE_SPAN;
  }

  /** Port of ColorModeSource::get(): raw mode-carrying value in, effective
   * 0x00RRGGBB out. Call only from the tick path (single-threaded). */
  resolve(raw: number): number {
    const modeByte = (raw >>> 24) & 0xff;
    // Unknown mode values (incl. 0xFF, the persisted pre-feature default)
    // are Static.
    const mode: ColorMode =
      modeByte >= ColorMode.SpectrumSweep && modeByte <= ColorMode.RandomTimerFade
        ? modeByte
        : ColorMode.Static;

    let reset = this.resetPending;
    this.resetPending = false;
    if (!this.stateValid || modeByte !== this.lastMode) {
      reset = true;
    }

    if (mode === ColorMode.Static) {
      this.stateValid = true;
      this.lastMode = modeByte;
      return raw & 0x00ffffff;
    }

    const speed = (raw >>> 16) & 0xff;
    const now = this.now();

    if (reset) {
      this.stateValid = true;
      this.lastMode = modeByte;
      this.lastNowMs = now;
      this.segmentStartMs = now;
      this.huePhase16 = 0;
      // Only the random modes consume entropy on reset; rolls start from the
      // previous hue so the first color of a new session still differs.
      if (mode !== ColorMode.SpectrumSweep) {
        this.currentHue = this.rollHueFrom(this.currentHue);
      }
      if (mode === ColorMode.RandomTimerFade) {
        this.prevHue = this.currentHue;
        this.targetHue = this.rollHueFrom(this.currentHue);
      }
    }

    switch (mode) {
      case ColorMode.SpectrumSweep: {
        const periodMs = sweepPeriodMs(speed);
        let dt = now - this.lastNowMs;
        if (dt < 0) {
          dt = 0;
        }
        this.lastNowMs = now;
        // Number-safe equivalent of the C++ uint64 phase math: dt and
        // periodMs are small, so dt * HUE_SPAN_Q16 stays far below 2^53.
        this.huePhase16 =
          (this.huePhase16 + Math.trunc((dt * HUE_SPAN_Q16) / periodMs)) % HUE_SPAN_Q16;
        return animColorFromHue(Math.trunc(this.huePhase16 / 65536));
      }

      case ColorMode.RandomOnBeat: {
        // No bound beat source degrades to RandomOnActivate: hold the color
        // rolled at reset.
        if (this.beatSource !== null && this.beatSource()) {
          this.currentHue = this.rollHueFrom(this.currentHue);
        }
        return animColorFromHue(this.currentHue);
      }

      case ColorMode.RandomOnActivate:
        return animColorFromHue(this.currentHue);

      case ColorMode.RandomTimerFade: {
        const intervalMs = timerIntervalMs(speed);
        let elapsed = now - this.segmentStartMs;
        if (elapsed < 0) {
          elapsed = 0;
        }
        if (elapsed >= intervalMs) {
          this.prevHue = this.targetHue;
          this.targetHue = this.rollHueFrom(this.targetHue);
          this.currentHue = this.targetHue;
          this.segmentStartMs = now;
          elapsed = 0;
        }
        const t256 = Math.trunc((elapsed * 256) / intervalMs);
        return animColorFromHue(hueLerp(this.prevHue, this.targetHue, t256));
      }

      default:
        return raw & 0x00ffffff;
    }
  }
}
