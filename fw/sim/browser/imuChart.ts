/**
 * Scrolling strip chart for the IMU card: three colored traces (X/Y/Z) of
 * one sensor (accel or gyro) over the last few seconds of SIM time, drawn
 * from TappedImuProvider's trace ring. Because the data is sim-time-stamped
 * 25 Hz deliveries, the chart shows exactly what the extension saw — manual
 * sources and scenario playback alike — scrolls only while the sim runs,
 * and holds still when paused.
 *
 * Rendering is dirty-checked: draw() is cheap to call every animation frame
 * and repaints only when new samples arrived or the canvas was resized.
 */

import type { ImuSample } from "../core/providers";
import type { ImuTracePoint } from "./sensors/imu";

const WINDOW_MS = 8000;

/* Fixed colors on purpose — --accent is retinted live from the framebuffer,
 * and trace identity must not drift with the animation's hue. */
const AXIS_COLORS = ["#ff6b6b", "#56d364", "#59a5ff"] as const; // X, Y, Z
const GRID_COLOR = "#262c38"; // --line
const TEXT_COLOR = "#8f9aad"; // --fg-dim

export class ImuStripChart {
  private readonly ctx: CanvasRenderingContext2D;
  private lastEndT = NaN;
  private lastCount = -1;
  private lastWidth = -1;

  /**
   * @param minRange Smallest full-scale half-range shown (chart autoscales
   *        outward from this so flat traces don't zoom into noise).
   * @param pick Selects this chart's vector from a sample.
   */
  constructor(
    private readonly canvas: HTMLCanvasElement,
    private readonly label: string,
    private readonly unit: string,
    private readonly minRange: number,
    private readonly pick: (s: ImuSample) => readonly [number, number, number],
  ) {
    const ctx = canvas.getContext("2d");
    if (ctx === null) {
      throw new Error("2d canvas context unavailable");
    }
    this.ctx = ctx;
  }

  draw(trace: readonly ImuTracePoint[]): void {
    const cssWidth = this.canvas.clientWidth;
    const cssHeight = this.canvas.clientHeight;
    if (cssWidth === 0 || cssHeight === 0) {
      return; // hidden tab — nothing to lay out against
    }
    const endT = trace.length > 0 ? trace[trace.length - 1].tMs : NaN;
    const clean =
      cssWidth === this.lastWidth &&
      trace.length === this.lastCount &&
      (endT === this.lastEndT || (Number.isNaN(endT) && Number.isNaN(this.lastEndT)));
    if (clean) {
      return;
    }
    this.lastWidth = cssWidth;
    this.lastCount = trace.length;
    this.lastEndT = endT;

    const dpr = window.devicePixelRatio || 1;
    const w = Math.round(cssWidth * dpr);
    const h = Math.round(cssHeight * dpr);
    if (this.canvas.width !== w || this.canvas.height !== h) {
      this.canvas.width = w;
      this.canvas.height = h;
    }
    const ctx = this.ctx;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, cssWidth, cssHeight);

    // Window anchored at the newest sample; scale symmetric about zero and
    // grown to fit the windowed data.
    const startT = endT - WINDOW_MS;
    let from = trace.length;
    let range = this.minRange;
    for (let i = trace.length - 1; i >= 0; i--) {
      if (trace[i].tMs < startT) {
        break;
      }
      from = i;
      const v = this.pick(trace[i].sample);
      range = Math.max(range, Math.abs(v[0]), Math.abs(v[1]), Math.abs(v[2]));
    }
    range *= 1.05;

    const midY = cssHeight / 2;
    const yOf = (v: number) => midY - (v / range) * (midY - 2);
    const xOf = (t: number) => ((t - startT) / WINDOW_MS) * cssWidth;

    // Zero line + 1 s ticks (sim time), so playback speed reads at a glance.
    ctx.strokeStyle = GRID_COLOR;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, midY);
    ctx.lineTo(cssWidth, midY);
    if (!Number.isNaN(endT)) {
      for (let t = Math.ceil(startT / 1000) * 1000; t <= endT; t += 1000) {
        ctx.moveTo(xOf(t), cssHeight - 5);
        ctx.lineTo(xOf(t), cssHeight);
      }
    }
    ctx.stroke();

    for (let axis = 0; axis < 3; axis++) {
      ctx.strokeStyle = AXIS_COLORS[axis];
      ctx.lineWidth = 1;
      ctx.beginPath();
      for (let i = from; i < trace.length; i++) {
        const x = xOf(trace[i].tMs);
        const y = yOf(this.pick(trace[i].sample)[axis]);
        if (i === from) {
          ctx.moveTo(x, y);
        } else {
          ctx.lineTo(x, y);
        }
      }
      ctx.stroke();
    }

    ctx.fillStyle = TEXT_COLOR;
    ctx.font = '10px ui-monospace, SFMono-Regular, "SF Mono", Menlo, Consolas, monospace';
    ctx.textBaseline = "top";
    const scale = range >= 10 ? range.toFixed(0) : range.toFixed(1);
    ctx.fillText(`${this.label}  ±${scale} ${this.unit}`, 4, 3);
  }
}
