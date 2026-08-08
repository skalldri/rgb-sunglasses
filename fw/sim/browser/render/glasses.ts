/**
 * Canvas renderer for the simulated panel.
 *
 * Geometry: the real hardware is ONE continuous PCB — a rounded panel with
 * a nose arch cut out of the bottom-center (see the CAD in the hardware
 * guide), not two separate lenses. The arch is DERIVED from the same
 * LEDS_ON_ROW table that defines the dead-cell mask (no duplicated
 * constants to drift): each contiguous band of equal dead-width
 * contributes a staircase corner, and the flank between corners is a
 * quadratic of the form x = E_k + (E_k+1 − E_k)·t², which stays within
 * the corner pair's x-range — so the rendered cutout is always a strict
 * SUPERSET of the dead-cell staircase. Board fill never covers a dead
 * (see-through) cell; between corners the smooth edge may recess slightly
 * behind LIVE rows, which only hides board face — sockets and LEDs draw
 * on top and are never lost.
 *
 * Light: lit LEDs render in three additive layers —
 *   1. a TIGHT glow buffer (6 px per cell, one cell of padding all round)
 *      holding each lit LED as a disc whose alpha tracks its own peak
 *      brightness — that keeps total glow energy ~quadratic in channel
 *      value, so the device-brightness (×0.02) preview stays honestly
 *      near-black like the real panel;
 *   2. a WIDE buffer produced by DOWNSCALING the tight one (no second
 *      per-disc rasterization), upscaled with bilinear smoothing for the
 *      soft outer halo;
 *   3. crisp LED cores, with an optional white-hot center (toggle):
 *      hardware-confirmed realism vs exact-hue debugging.
 * Both buffers upscale onto a destination rect padded by one cell, so
 * perimeter-LED bloom spills naturally onto the bezel instead of clipping
 * in a straight line at the grid edge. Additive ('lighter') compositing
 * makes overlapping glows SUM like real light.
 *
 * Performance: the static layer renders once per resize; per frame it's
 * ONE disc pass into the tight buffer, one small downscale blit, two
 * upscale drawImages, and 1-2 arc fills per lit LED. Measured on the
 * shared Pixel 9 Pro (Chrome, dev server) BEFORE the single-pass
 * optimization: full-field plasma (all 432 LEDs lit, white-hot on) with
 * display decimation OFF sustained 89 fps against the ~91 fps pacing
 * target; the current path does strictly less work per frame.
 */

import { DISPLAY_HEIGHT, DISPLAY_WIDTH, LEDS_ON_ROW, LIVE_MASK } from "../../core/display";

/** Horizontal / vertical padding around the LED grid, in grid pitches.
 * PAIRED with .canvas-wrap's aspect-ratio in style.css — keep
 * (40+PAD_X)/(12+PAD_Y) equal to that ratio so the panel fills the box
 * and nothing (temple arms included) draws outside the canvas. */
const PAD_X = 3.4;
const PAD_Y = 2.8;

/** How far the temple-arm stubs reach beyond the panel edge, in pitches.
 * Must stay under PAD_X/2 (the canvas margin) plus the line cap. */
const ARM_REACH = 1.4;

/** Offscreen glow buffer resolutions, in pixels per LED cell, and the
 * one-cell padding that lets edge bloom escape the grid rectangle. */
const TIGHT_PX_PER_CELL = 6;
const WIDE_PX_PER_CELL = 3;
const GLOW_PAD_CELLS = 1;

/** One staircase corner of the nose arch, in grid-EDGE units: the dead
 * region spans [leftEdge, rightEdge] for rows topRow..(next band). */
interface ArchBand {
  leftEdge: number;
  rightEdge: number;
  topRow: number;
}

/** Derives the arch bands from LEDS_ON_ROW, bottom-up. For proto0 this
 * yields [{15,25,10}, {16,24,8}, {17,23,6}]. A future panel revision that
 * changes LEDS_ON_ROW reshapes the outline automatically. */
function deriveArchBands(): ArchBand[] {
  const bands: ArchBand[] = [];
  for (let y = DISPLAY_HEIGHT - 1; y >= 0; y--) {
    const missing = DISPLAY_WIDTH / 2 - LEDS_ON_ROW[y];
    if (missing <= 0) {
      break;
    }
    const leftEdge = LEDS_ON_ROW[y];
    const rightEdge = DISPLAY_WIDTH - LEDS_ON_ROW[y];
    const last = bands[bands.length - 1];
    if (last !== undefined && last.leftEdge === leftEdge) {
      last.topRow = y; // extend the band upward
    } else {
      bands.push({ leftEdge, rightEdge, topRow: y });
    }
  }
  return bands;
}

const ARCH_BANDS = deriveArchBands();

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
    if (ctx === null || bgCtx === null) {
      throw new Error("2D canvas context unavailable");
    }
    this.ctx = ctx;
    this.bgCtx = bgCtx;
    ({ canvas: this.glowTight, ctx: this.glowTightCtx } = makeGlowBuffer(TIGHT_PX_PER_CELL));
    ({ canvas: this.glowWide, ctx: this.glowWideCtx } = makeGlowBuffer(WIDE_PX_PER_CELL));
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
   * arch (see the header for the superset invariant). One path — fill
   * covers the board, so clipping to it also excludes the arch. */
  private panelPath(c: CanvasRenderingContext2D): void {
    const p = this.pitch;
    const left = this.edgeX(0) - p * 0.9;
    const right = this.edgeX(DISPLAY_WIDTH) + p * 0.9;
    const top = this.edgeY(0) - p * 0.85;
    const bottom = this.edgeY(DISPLAY_HEIGHT) + p * 0.85;
    const r = p * 1.15;

    const bands = ARCH_BANDS;
    const innermost = bands[bands.length - 1];
    const crownY = this.edgeY(innermost.topRow) - p * 0.15;

    c.beginPath();
    c.moveTo(left + r, top);
    c.arcTo(right, top, right, bottom, r);
    c.arcTo(right, bottom, left, bottom, r);

    // Bottom edge, right to left, diverting up into the nose arch.
    // Right flank, bottom-up: per staircase band, a quadratic whose
    // control point pins x to the outer corner (x = E_k + ΔE·t²), so the
    // flank never crosses inside a band's dead edge.
    const first = bands[0];
    c.lineTo(this.edgeX(first.rightEdge) + p * 0.45, bottom);
    let prevX = this.edgeX(first.rightEdge);
    let prevY = bottom;
    c.quadraticCurveTo(prevX, bottom, prevX, this.edgeY(first.topRow));
    prevY = this.edgeY(first.topRow);
    for (let k = 1; k < bands.length; k++) {
      const nx = this.edgeX(bands[k].rightEdge);
      const ny = this.edgeY(bands[k].topRow);
      c.quadraticCurveTo(prevX, (prevY + ny) / 2, nx, ny);
      prevX = nx;
      prevY = ny;
    }
    // Crown: gently rounded across the innermost band's top.
    c.quadraticCurveTo(prevX, crownY, (prevX + this.edgeX(innermost.leftEdge)) / 2, crownY);
    c.quadraticCurveTo(this.edgeX(innermost.leftEdge), crownY, this.edgeX(innermost.leftEdge), this.edgeY(innermost.topRow));
    // Left flank, top-down (mirror of the right).
    prevX = this.edgeX(innermost.leftEdge);
    prevY = this.edgeY(innermost.topRow);
    for (let k = bands.length - 2; k >= 0; k--) {
      const nx = this.edgeX(bands[k].leftEdge);
      const ny = this.edgeY(bands[k].topRow);
      c.quadraticCurveTo(prevX, (prevY + ny) / 2, nx, ny);
      prevX = nx;
      prevY = ny;
    }
    c.quadraticCurveTo(prevX, bottom, prevX, bottom);
    c.lineTo(this.edgeX(first.leftEdge) - p * 0.45, bottom);

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

    // Temple arm stubs behind the panel's top outer corners (reach bounded
    // by ARM_REACH so they stay inside the canvas margin — see PAD_X).
    c.strokeStyle = "#1b2029";
    c.lineWidth = p * 0.5;
    c.lineCap = "round";
    const armY = this.edgeY(0) - p * 0.2;
    c.beginPath();
    c.moveTo(this.edgeX(0) - p * 0.7, armY);
    c.lineTo(this.edgeX(0) - p * ARM_REACH, armY + p * 0.3);
    c.moveTo(this.edgeX(DISPLAY_WIDTH) + p * 0.7, armY);
    c.lineTo(this.edgeX(DISPLAY_WIDTH) + p * ARM_REACH, armY + p * 0.3);
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

  /** Paints one displayed frame (already dead-masked and brightness-scaled
   * by toDisplayedFrame — this function does no colour math of its own). */
  draw(frame: Uint8Array): void {
    const c = this.ctx;
    c.clearRect(0, 0, this.cssWidth, this.cssHeight);
    c.drawImage(this.bg, 0, 0, this.cssWidth, this.cssHeight);

    // Glow pass: rasterize lit discs ONCE into the tight buffer; the wide
    // buffer is a downscale of it. Per-disc alpha tracks the LED's own
    // peak, keeping total glow energy ~quadratic in channel value — the
    // device-brightness preview stays honestly near-black.
    const tctx = this.glowTightCtx;
    tctx.clearRect(0, 0, this.glowTight.width, this.glowTight.height);
    let anyLit = false;
    forEachLit(frame, (i, gx, gy, r, g, b) => {
      anyLit = true;
      const peak = Math.max(r, g, b);
      tctx.fillStyle = `rgba(${r},${g},${b},${peak / 255})`;
      tctx.beginPath();
      tctx.arc(
        (gx + GLOW_PAD_CELLS + 0.5) * TIGHT_PX_PER_CELL,
        (gy + GLOW_PAD_CELLS + 0.5) * TIGHT_PX_PER_CELL,
        2.9,
        0,
        Math.PI * 2,
      );
      tctx.fill();
    });

    if (anyLit) {
      const wctx = this.glowWideCtx;
      wctx.clearRect(0, 0, this.glowWide.width, this.glowWide.height);
      wctx.imageSmoothingEnabled = true;
      wctx.drawImage(this.glowTight, 0, 0, this.glowWide.width, this.glowWide.height);

      // Destination rect includes the buffers' one-cell padding, so edge
      // bloom spills onto the bezel instead of clipping at the grid.
      const dx = this.originX - GLOW_PAD_CELLS * this.pitch;
      const dy = this.originY - GLOW_PAD_CELLS * this.pitch;
      const dw = (DISPLAY_WIDTH + 2 * GLOW_PAD_CELLS) * this.pitch;
      const dh = (DISPLAY_HEIGHT + 2 * GLOW_PAD_CELLS) * this.pitch;
      c.save();
      c.globalCompositeOperation = "lighter";
      c.imageSmoothingEnabled = true;
      c.imageSmoothingQuality = "high";
      // Wide, faint outer glow first...
      c.globalAlpha = 0.55;
      c.drawImage(this.glowWide, dx, dy, dw, dh);
      // ...then the tighter, brighter halo.
      c.globalAlpha = 0.85;
      c.drawImage(this.glowTight, dx, dy, dw, dh);
      c.restore();
    }

    // Crisp LED dies on top. Two modes, user-toggleable:
    //  - whiteHot ON (default): a white-hot center grows with intensity —
    //    matches what the eye sees on the REAL panel (hardware-confirmed);
    //  - whiteHot OFF: the core is the EXACT frame color, for debugging
    //    the hue an extension computed without the wash-out masking it
    //    (the same class of confusion as the issue #259 hue drift).
    const coreR = this.dotR * 0.8;
    const hotR = this.dotR * 0.42;
    forEachLit(frame, (i, gx, gy, r, g, b) => {
      const x = this.cellX(gx);
      const y = this.cellY(gy);
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
    });
  }
}

/** Iterates the lit, live LEDs of a frame — the single definition of
 * "which pixels emit light", shared by the glow and core passes so they
 * can never disagree. */
function forEachLit(
  frame: Uint8Array,
  cb: (i: number, gx: number, gy: number, r: number, g: number, b: number) => void,
): void {
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
    cb(i, i % DISPLAY_WIDTH, (i / DISPLAY_WIDTH) | 0, r, g, b);
  }
}

/** Creates one padded offscreen glow buffer at `pxPerCell` resolution. */
function makeGlowBuffer(pxPerCell: number): {
  canvas: HTMLCanvasElement;
  ctx: CanvasRenderingContext2D;
} {
  const canvas = document.createElement("canvas");
  canvas.width = (DISPLAY_WIDTH + 2 * GLOW_PAD_CELLS) * pxPerCell;
  canvas.height = (DISPLAY_HEIGHT + 2 * GLOW_PAD_CELLS) * pxPerCell;
  const ctx = canvas.getContext("2d");
  if (ctx === null) {
    throw new Error("2D canvas context unavailable");
  }
  return { canvas, ctx };
}

/** Mixes a channel toward white by factor m in [0, 1]. */
function mix(channel: number, m: number): number {
  return Math.round(channel + (255 - channel) * m);
}

function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}
