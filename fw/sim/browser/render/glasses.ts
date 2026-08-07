/**
 * Canvas renderer for the simulated panel: 40x12 round LEDs laid out as two
 * lenses with the nose cutout, inside a dark bezel.
 *
 * The dead cells (LIVE_MASK == 0) are simply not drawn — no socket, no dot —
 * so the staircase-shaped nose cutout emerges from the real geometry rather
 * than from a hardcoded rectangle.
 *
 * Performance: everything static (bezel, lens plates, unlit sockets) is
 * rendered once into an offscreen canvas and blitted per frame; only lit
 * LEDs cost per-frame work, at two arc fills each. That keeps a full 432-LED
 * frame well inside the ~11 ms tick budget even without display decimation.
 */

import { DISPLAY_HEIGHT, DISPLAY_WIDTH, LIVE_MASK } from "../../core/display";

/** Horizontal / vertical padding around the LED grid, in grid pitches. */
const PAD_X = 3.2;
const PAD_Y = 4.6;

export class GlassesRenderer {
  private readonly ctx: CanvasRenderingContext2D;
  private readonly bg: HTMLCanvasElement;
  private readonly bgCtx: CanvasRenderingContext2D;

  private pitch = 0;
  private dotR = 0;
  private originX = 0;
  private originY = 0;
  private cssWidth = 0;
  private cssHeight = 0;

  constructor(private readonly canvas: HTMLCanvasElement) {
    const ctx = canvas.getContext("2d");
    const bg = document.createElement("canvas");
    const bgCtx = bg.getContext("2d");
    if (ctx === null || bgCtx === null) {
      throw new Error("2D canvas context unavailable");
    }
    this.ctx = ctx;
    this.bg = bg;
    this.bgCtx = bgCtx;
    this.resize();
  }

  /** Maps a canvas-relative CSS point to normalized grid coords in [-1, 1],
   * (0, 0) at the centre of the panel — used by the mouse-tilt IMU source. */
  normalizedPoint(clientX: number, clientY: number): { nx: number; ny: number } {
    const rect = this.canvas.getBoundingClientRect();
    const gridW = this.pitch * DISPLAY_WIDTH;
    const gridH = this.pitch * DISPLAY_HEIGHT;
    const x = clientX - rect.left - this.originX;
    const y = clientY - rect.top - this.originY;
    return {
      nx: clamp((x / gridW) * 2 - 1, -1, 1),
      ny: clamp((y / gridH) * 2 - 1, -1, 1),
    };
  }

  /** Re-measures the element and rebuilds the cached static layer. Safe to
   * call on every ResizeObserver notification — it early-outs when nothing
   * actually changed. */
  resize(): void {
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.floor(this.canvas.clientWidth));
    const h = Math.max(1, Math.floor(this.canvas.clientHeight));
    if (w === this.cssWidth && h === this.cssHeight && this.canvas.width === w * dpr) {
      return;
    }
    this.cssWidth = w;
    this.cssHeight = h;
    this.canvas.width = Math.floor(w * dpr);
    this.canvas.height = Math.floor(h * dpr);
    this.bg.width = this.canvas.width;
    this.bg.height = this.canvas.height;
    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    this.bgCtx.setTransform(dpr, 0, 0, dpr, 0, 0);

    this.pitch = Math.min(w / (DISPLAY_WIDTH + PAD_X), h / (DISPLAY_HEIGHT + PAD_Y));
    this.dotR = this.pitch * 0.4;
    this.originX = (w - this.pitch * DISPLAY_WIDTH) / 2;
    this.originY = (h - this.pitch * DISPLAY_HEIGHT) / 2;
    this.paintStatic();
  }

  private cellX(x: number): number {
    return this.originX + (x + 0.5) * this.pitch;
  }

  private cellY(y: number): number {
    return this.originY + (y + 0.5) * this.pitch;
  }

  /** Bezel + lens plates + unlit sockets, drawn once per resize. */
  private paintStatic(): void {
    const c = this.bgCtx;
    const p = this.pitch;
    c.clearRect(0, 0, this.cssWidth, this.cssHeight);
    c.fillStyle = "#07080b";
    c.fillRect(0, 0, this.cssWidth, this.cssHeight);

    // One lens per LED bank; each is the bounding box of its own cells,
    // padded out to leave a rim of bezel around the outermost LEDs.
    const lensPad = p * 0.75;
    const lenses = [
      { x0: this.cellX(0), x1: this.cellX(DISPLAY_WIDTH / 2 - 1) },
      { x0: this.cellX(DISPLAY_WIDTH / 2), x1: this.cellX(DISPLAY_WIDTH - 1) },
    ];
    const top = this.cellY(0) - lensPad;
    const bottom = this.cellY(DISPLAY_HEIGHT - 1) + lensPad;

    // Temple arms, drawn first so the lenses sit on top of them.
    c.strokeStyle = "#1b2029";
    c.lineWidth = p * 0.55;
    c.lineCap = "round";
    for (const [outerX, dir] of [
      [lenses[0].x0 - lensPad, -1],
      [lenses[1].x1 + lensPad, 1],
    ] as const) {
      c.beginPath();
      c.moveTo(outerX, top + p * 0.6);
      c.lineTo(outerX + dir * p * 1.6, top + p * 0.9);
      c.stroke();
    }

    // Nose bridge: a bar spanning the gap between the two lenses, at the
    // top of the panel (the cutout itself is at the bottom).
    c.fillStyle = "#12161d";
    const bridgeLeft = lenses[0].x1 + lensPad;
    const bridgeRight = lenses[1].x0 - lensPad;
    if (bridgeRight > bridgeLeft) {
      c.fillRect(bridgeLeft, top + p * 0.4, bridgeRight - bridgeLeft, p * 0.9);
    }

    for (const lens of lenses) {
      const x = lens.x0 - lensPad;
      const y = top;
      const w = lens.x1 - lens.x0 + lensPad * 2;
      const h = bottom - top;
      roundRect(c, x, y, w, h, p * 1.4);
      const grad = c.createLinearGradient(x, y, x, y + h);
      grad.addColorStop(0, "#171c25");
      grad.addColorStop(1, "#0c0f15");
      c.fillStyle = grad;
      c.fill();
      c.strokeStyle = "rgba(140, 165, 210, 0.22)";
      c.lineWidth = Math.max(1, p * 0.09);
      c.stroke();
    }

    // Unlit sockets. Dead cells get nothing at all.
    c.fillStyle = "#1c212b";
    for (let y = 0; y < DISPLAY_HEIGHT; y++) {
      for (let x = 0; x < DISPLAY_WIDTH; x++) {
        if (LIVE_MASK[y * DISPLAY_WIDTH + x] === 0) {
          continue;
        }
        c.beginPath();
        c.arc(this.cellX(x), this.cellY(y), this.dotR, 0, Math.PI * 2);
        c.fill();
      }
    }
  }

  /** Paints one displayed frame (already dead-masked and brightness-scaled
   * by toDisplayedFrame — this function does no colour math of its own). */
  draw(frame: Uint8Array): void {
    const c = this.ctx;
    c.clearRect(0, 0, this.cssWidth, this.cssHeight);
    c.drawImage(this.bg, 0, 0, this.cssWidth, this.cssHeight);

    // Pass 1: additive halos, so overlapping neighbours bloom together the
    // way diffused LEDs do. Alpha tracks the LED's own brightness, which
    // keeps the device-brightness view honestly dim.
    c.globalCompositeOperation = "lighter";
    const haloR = this.dotR * 2.1;
    for (let i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++) {
      if (LIVE_MASK[i] === 0) {
        continue;
      }
      const r = frame[i * 3];
      const g = frame[i * 3 + 1];
      const b = frame[i * 3 + 2];
      const peak = Math.max(r, g, b);
      if (peak === 0) {
        continue;
      }
      c.fillStyle = `rgba(${r},${g},${b},${(0.5 * peak) / 255})`;
      c.beginPath();
      c.arc(this.cellX(i % DISPLAY_WIDTH), this.cellY((i / DISPLAY_WIDTH) | 0), haloR, 0, Math.PI * 2);
      c.fill();
    }

    // Pass 2: the LED die itself.
    c.globalCompositeOperation = "source-over";
    const coreR = this.dotR * 0.86;
    for (let i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++) {
      if (LIVE_MASK[i] === 0) {
        continue;
      }
      const r = frame[i * 3];
      const g = frame[i * 3 + 1];
      const b = frame[i * 3 + 2];
      if ((r | g | b) === 0) {
        continue;
      }
      c.fillStyle = `rgb(${r},${g},${b})`;
      c.beginPath();
      c.arc(this.cellX(i % DISPLAY_WIDTH), this.cellY((i / DISPLAY_WIDTH) | 0), coreR, 0, Math.PI * 2);
      c.fill();
    }
  }
}

function roundRect(
  c: CanvasRenderingContext2D,
  x: number,
  y: number,
  w: number,
  h: number,
  r: number,
): void {
  const rad = Math.min(r, w / 2, h / 2);
  c.beginPath();
  c.moveTo(x + rad, y);
  c.arcTo(x + w, y, x + w, y + h, rad);
  c.arcTo(x + w, y + h, x, y + h, rad);
  c.arcTo(x, y + h, x, y, rad);
  c.arcTo(x, y, x + w, y, rad);
  c.closePath();
}

function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}
