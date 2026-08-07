/**
 * Display geometry and the device brightness model.
 *
 * Mirrors fw/src/led_config.h (kFrame* constants) and the pixel-population
 * rule in fw/src/led_controller.cpp set_pixel_in_framebuffer(): bank 0
 * (x < 20) is missing LEDs on the RIGHT of each short row, bank 1 (x >= 20)
 * on the LEFT. 48 of the 480 logical cells have no LED (the nose cutout).
 */

export const DISPLAY_WIDTH = 40;
export const DISPLAY_HEIGHT = 12;
export const BANK_WIDTH = 20;
export const FRAME_BYTES = DISPLAY_WIDTH * DISPLAY_HEIGHT * 3;

/** LEDs populated per row of ONE bank (kFrameLedsOnRow). */
export const LEDS_ON_ROW = [20, 20, 20, 20, 20, 20, 17, 17, 16, 16, 15, 15] as const;

/** Advisory nose-cutout rectangle (kFrameNoseCutout*) — a superset of the
 * true missing set; the authoritative rule is isDeadPixel(). */
export const NOSE_CUTOUT = { x: 15, y: 6, width: 10, height: 6 } as const;

/** Default global brightness factor: coreBrightness/1000 with the
 * persisted default of 20 (core_config.cpp). */
export const DEFAULT_BRIGHTNESS_FACTOR = 20 / 1000;

/** True if logical cell (x, y) has no physical LED. */
export function isDeadPixel(x: number, y: number): boolean {
  const ledsOnRow = LEDS_ON_ROW[y];
  const missing = BANK_WIDTH - ledsOnRow;
  if (x < BANK_WIDTH) {
    return x >= ledsOnRow; // bank 0: right side of the row is missing
  }
  return x - BANK_WIDTH < missing; // bank 1: left side is missing
}

/** 480-entry mask, 1 = live LED, 0 = dead cell. Index y*40+x. */
export const LIVE_MASK: Uint8Array = (() => {
  const mask = new Uint8Array(DISPLAY_WIDTH * DISPLAY_HEIGHT);
  for (let y = 0; y < DISPLAY_HEIGHT; y++) {
    for (let x = 0; x < DISPLAY_WIDTH; x++) {
      mask[y * DISPLAY_WIDTH + x] = isDeadPixel(x, y) ? 0 : 1;
    }
  }
  return mask;
})();

export const LIVE_PIXEL_COUNT: number = LIVE_MASK.reduce((a, b) => a + b, 0);

/**
 * Applies the pattern controller's global-brightness step to one channel:
 * float multiply then integer TRUNCATION (pattern_controller.cpp scales by
 * sBrightnessForFrame and the float->uint8 conversion truncates). The
 * truncation is visually load-bearing — it is why saturated hues drift as
 * they dim on real hardware (issue #259) — so keep it exact.
 */
export function applyBrightness(channel: number, factor: number): number {
  return Math.trunc(channel * factor);
}

/**
 * Copies a raw extension framebuffer to "what the wearer sees": dead cells
 * forced black, every channel brightness-scaled-and-truncated. Pass
 * factor=1 to keep full scale (dead cells are still masked — they do not
 * exist at any brightness).
 */
export function toDisplayedFrame(raw: Uint8Array, factor: number): Uint8Array {
  const out = new Uint8Array(FRAME_BYTES);
  for (let i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++) {
    if (LIVE_MASK[i] === 0) {
      continue;
    }
    out[i * 3] = applyBrightness(raw[i * 3], factor);
    out[i * 3 + 1] = applyBrightness(raw[i * 3 + 1], factor);
    out[i * 3 + 2] = applyBrightness(raw[i * 3 + 2], factor);
  }
  return out;
}
