/**
 * Canvas renderer for the simulated panel.
 *
 * Geometry: the real hardware is ONE continuous PCB — a rounded panel with
 * a nose arch cut out of the bottom-center (see the CAD in the hardware
 * guide), not two separate lenses. The arch flanks are smooth curves
 * anchored to the dead-cell geometry of LIVE_MASK at every width
 * transition (edges 15/25 at the bottom, 16/24 at rows 8-9, 17/23 at rows
 * 6-7). Between anchors the smooth flank averages the staircase — which
 * matches the physical truth: the PCB edge is a smooth curve, and the
 * dead-cell staircase is the LED grid's approximation of it.
 *
 * Light: lit LEDs render in three additive layers —
 *   1. a WIDE soft glow: all lit LEDs drawn into a tiny offscreen buffer
 *      (3 px per cell) and upscaled with bilinear smoothing, which acts as
 *      a cheap, universally-supported gaussian-ish blur (no ctx.filter —
 *      Safari support is spotty);
 *   2. a TIGHT glow from a second buffer at 6 px per cell for the bright
 *      halo right around the die;
 *   3. crisp LED cores, with a white-hot center that grows with intensity
 *      (a saturated LED reads white at the die, colored in the halo — like
 *      the real thing).
 * Additive ('lighter') compositing makes overlapping glows SUM — adjacent
 * lit LEDs merge into a continuous light bar instead of stacking flat
 * alpha discs into moiré-like patterns.
 *
 * Performance: the static layer (panel, slots, sockets) renders once per
 * resize; per frame it's two small-buffer disc passes, two drawImages, and
 * up to three arc fills per lit LED. Measured on the shared Pixel 9 Pro
 * (Chrome, dev server): full-field plasma (all 432 LEDs lit, white-hot on)
 * with display decimation OFF sustains 89 fps against the ~91 fps pacing
 * target — the worst case fits the per-tick budget without decimation.
 */

import { DISPLAY_HEIGHT, DISPLAY_WIDTH, LIVE_MASK } from "../../core/display";

/** Horizontal / vertical padding around the LED grid, in grid pitches. */
const PAD_X = 2.6;
const PAD_Y = 2.6;

/** Nose arch, in grid-EDGE units (cell k spans edges k..k+1). Anchored to
 * the dead-cell rows in core/display.ts at every width transition: rows
 * 10-11 miss cells 15..24 (edges 15/25), rows 8-9 miss 16..23 (edges
 * 16/24), rows 6-7 miss 17..22 (edges 17/23). The flanks pass through all
 * three anchor pairs, so the board fill never covers a dead cell's edge. */
const ARCH_BOTTOM_LEFT = 15;
const ARCH_BOTTOM_RIGHT = 25;
const ARCH_MID_LEFT = 16;
const ARCH_MID_RIGHT = 24;
const ARCH_MID_ROW = 9;
const ARCH_TOP_LEFT = 17;
const ARCH_TOP_RIGHT = 23;
const ARCH_TOP_ROW = 6;

/** Offscreen glow buffer resolutions, in pixels per LED cell. */
const TIGHT_PX_PER_CELL = 6;
const WIDE_PX_PER_CELL = 3;

export class GlassesRenderer {
  private readonly ctx: CanvasRenderingContext2D;
  private readonly bg: HTMLCanvasElement;
  private readonly bgCtx: CanvasRenderingContext2D;
  private readonly glowTight: HTMLCanvasElement;
  private readonly glowTightCtx: CanvasRenderingContext2D;
  private readonly glowWide: HTMLCanvasElement;
  private readonly glowWideCtx: CanvasRenderingContext2D;

  /** White-out saturated LED centers like the real panel does to the eye
   * (hardware-confirmed). Turn OFF to read exact computed hues. */
  whiteHot = true;

  private pitch = 0;
  private dotR = 0;
  private originX = 0;
  private originY = 0;
  private cssWidth = 0;
  private cssHeight = 0;

  constructor(private readonly canvas: HTMLCanvasElement) {
    const ctx = canvas.getContext("2d");
    this.bg = document.createElement("canvas");
    const bgCtx = this.bg.getContext("2d");
    this.glowTight = document.createElement("canvas");
    this.glowTight.width = DISPLAY_WIDTH * TIGHT_PX_PER_CELL;
    this.glowTight.height = DISPLAY_HEIGHT * TIGHT_PX_PER_CELL;
    const tightCtx = this.glowTight.getContext("2d");
    this.glowWide = document.createElement("canvas");
    this.glowWide.width = DISPLAY_WIDTH * WIDE_PX_PER_CELL;
    this.glowWide.height = DISPLAY_HEIGHT * WIDE_PX_PER_CELL;
    const wideCtx = this.glowWide.getContext("2d");
    if (ctx === null || bgCtx === null || tightCtx === null || wideCtx === null) {
      throw new Error("2D canvas context unavailable");
    }
    this.ctx = ctx;
    this.bgCtx = bgCtx;
    this.glowTightCtx = tightCtx;
    this.glowWideCtx = wideCtx;
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
    this.dotR = this.pitch * 0.38;
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

  /** Grid-EDGE coordinate to pixels (edge k = left/top edge of cell k). */
  private edgeX(gx: number): number {
    return this.originX + gx * this.pitch;
  }

  private edgeY(gy: number): number {
    return this.originY + gy * this.pitch;
  }

  /** Traces the PCB outline: rounded panel with the bottom-center nose
   * arch. One path — fill covers the board, so clipping to it also
   * excludes the arch. */
  private panelPath(c: CanvasRenderingContext2D): void {
    const p = this.pitch;
    const left = this.edgeX(0) - p * 0.9;
    const right = this.edgeX(DISPLAY_WIDTH) + p * 0.9;
    const top = this.edgeY(0) - p * 0.85;
    const bottom = this.edgeY(DISPLAY_HEIGHT) + p * 0.85;
    const r = p * 1.15;

    const aRB = this.edgeX(ARCH_BOTTOM_RIGHT);
    const aRM = this.edgeX(ARCH_MID_RIGHT);
    const aRT = this.edgeX(ARCH_TOP_RIGHT);
    const aLT = this.edgeX(ARCH_TOP_LEFT);
    const aLM = this.edgeX(ARCH_MID_LEFT);
    const aLB = this.edgeX(ARCH_BOTTOM_LEFT);
    const aMidY = this.edgeY(ARCH_MID_ROW);
    const aTop = this.edgeY(ARCH_TOP_ROW) - p * 0.15;

    c.beginPath();
    c.moveTo(left + r, top);
    c.arcTo(right, top, right, bottom, r);
    c.arcTo(right, bottom, left, bottom, r);
    // Bottom edge, right to left, diverting up into the nose arch. Each
    // flank runs through the mid-row anchor (edges 16/24 at rows 8-9) so
    // the fill tracks the true dead-cell staircase; the crown is rounded —
    // the CAD's silhouette.
    c.lineTo(aRB + p * 0.45, bottom);
    c.quadraticCurveTo(aRB, bottom, aRM, aMidY);
    c.quadraticCurveTo(aRT, aTop, (aRT + aLT) / 2, aTop);
    c.quadraticCurveTo(aLT, aTop, aLM, aMidY);
    c.quadraticCurveTo(aLB, bottom, aLB - p * 0.45, bottom);
    c.arcTo(left, bottom, left, top, r);
    c.arcTo(left, top, right, top, r);
    c.closePath();
  }

  /** Panel + slot texture + unlit sockets, drawn once per resize. */
  private paintStatic(): void {
    const c = this.bgCtx;
    const p = this.pitch;
    c.clearRect(0, 0, this.cssWidth, this.cssHeight);
    c.fillStyle = "#07080b";
    c.fillRect(0, 0, this.cssWidth, this.cssHeight);

    // Temple arm stubs behind the panel's top outer corners.
    c.strokeStyle = "#1b2029";
    c.lineWidth = p * 0.5;
    c.lineCap = "round";
    const armY = this.edgeY(0) - p * 0.2;
    c.beginPath();
    c.moveTo(this.edgeX(0) - p * 0.7, armY);
    c.lineTo(this.edgeX(0) - p * 2.2, armY + p * 0.35);
    c.moveTo(this.edgeX(DISPLAY_WIDTH) + p * 0.7, armY);
    c.lineTo(this.edgeX(DISPLAY_WIDTH) + p * 2.2, armY + p * 0.35);
    c.stroke();

    // The board itself.
    this.panelPath(c);
    const grad = c.createLinearGradient(0, this.edgeY(0), 0, this.edgeY(DISPLAY_HEIGHT));
    grad.addColorStop(0, "#161b24");
    grad.addColorStop(1, "#0d1016");
    c.fillStyle = grad;
    c.fill();
    c.strokeStyle = "rgba(140, 165, 210, 0.25)";
    c.lineWidth = Math.max(1, p * 0.09);
    c.stroke();

    // See-through slots between LED rows (the CAD's horizontal cutouts) —
    // clipped to the panel so they never cross the nose arch.
    c.save();
    this.panelPath(c);
    c.clip();
    c.fillStyle = "rgba(0, 0, 0, 0.35)";
    for (let gy = 1; gy < DISPLAY_HEIGHT; gy++) {
      const y = this.edgeY(gy);
      c.fillRect(this.edgeX(0) - p * 0.4, y - p * 0.07, DISPLAY_WIDTH * p + p * 0.8, p * 0.14);
    }
    c.restore();

    // Unlit sockets. Dead cells get nothing at all.
    c.fillStyle = "#1e242f";
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

  /** Renders lit LEDs as discs into one of the glow buffers. Color carries
   * the brightness (the frame is already scaled), so the buffer holds the
   * light energy the upscale then diffuses. */
  private renderGlowBuffer(
    c: CanvasRenderingContext2D,
    frame: Uint8Array,
    pxPerCell: number,
    radius: number,
  ): boolean {
    c.clearRect(0, 0, DISPLAY_WIDTH * pxPerCell, DISPLAY_HEIGHT * pxPerCell);
    let anyLit = false;
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
      anyLit = true;
      const cx = ((i % DISPLAY_WIDTH) + 0.5) * pxPerCell;
      const cy = (((i / DISPLAY_WIDTH) | 0) + 0.5) * pxPerCell;
      c.fillStyle = `rgb(${r},${g},${b})`;
      c.beginPath();
      c.arc(cx, cy, radius, 0, Math.PI * 2);
      c.fill();
    }
    return anyLit;
  }

  /** Paints one displayed frame (already dead-masked and brightness-scaled
   * by toDisplayedFrame — this function does no colour math of its own). */
  draw(frame: Uint8Array): void {
    const c = this.ctx;
    c.clearRect(0, 0, this.cssWidth, this.cssHeight);
    c.drawImage(this.bg, 0, 0, this.cssWidth, this.cssHeight);

    const gridX = this.originX;
    const gridY = this.originY;
    const gridW = this.pitch * DISPLAY_WIDTH;
    const gridH = this.pitch * DISPLAY_HEIGHT;

    // Glow passes: tiny buffers upscaled with bilinear smoothing = cheap
    // blur; 'lighter' makes overlapping halos ADD like real light.
    const anyTight = this.renderGlowBuffer(this.glowTightCtx, frame, TIGHT_PX_PER_CELL, 2.9);
    if (anyTight) {
      this.renderGlowBuffer(this.glowWideCtx, frame, WIDE_PX_PER_CELL, 2.1);
      c.save();
      c.globalCompositeOperation = "lighter";
      c.imageSmoothingEnabled = true;
      c.imageSmoothingQuality = "high";
      // Wide, faint outer glow first...
      c.globalAlpha = 0.5;
      c.drawImage(this.glowWide, gridX, gridY, gridW, gridH);
      // ...then the tighter, brighter halo.
      c.globalAlpha = 0.85;
      c.drawImage(this.glowTight, gridX, gridY, gridW, gridH);
      c.restore();
    }

    // Crisp LED dies on top. Two modes, user-toggleable:
    //  - whiteHot ON (default): a white-hot center grows with intensity —
    //    matches what the eye/camera sees on the REAL panel, where a
    //    saturated die washes out to white (hardware-confirmed);
    //  - whiteHot OFF: the core is the EXACT frame color, for debugging
    //    the hue an extension computed without the wash-out masking it
    //    (the same class of confusion as the issue #259 hue drift).
    const coreR = this.dotR * 0.8;
    const hotR = this.dotR * 0.42;
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
      const x = this.cellX(i % DISPLAY_WIDTH);
      const y = this.cellY((i / DISPLAY_WIDTH) | 0);
      c.fillStyle = `rgb(${r},${g},${b})`;
      c.beginPath();
      c.arc(x, y, coreR, 0, Math.PI * 2);
      c.fill();

      if (this.whiteHot) {
        const peak = Math.max(r, g, b);
        const hot = (peak - 150) / 105;
        if (hot > 0) {
          const m = hot * hot * 0.9;
          c.fillStyle = `rgb(${mix(r, m)},${mix(g, m)},${mix(b, m)})`;
          c.beginPath();
          c.arc(x, y, hotR, 0, Math.PI * 2);
          c.fill();
        }
      }
    }
  }
}

/** Mixes a channel toward white by factor m in [0, 1]. */
function mix(channel: number, m: number): number {
  return Math.round(channel + (255 - channel) * m);
}

function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}
