import { test } from "node:test";
import assert from "node:assert/strict";
import {
  DISPLAY_HEIGHT,
  DISPLAY_WIDTH,
  LIVE_MASK,
  LIVE_PIXEL_COUNT,
  applyBrightness,
  isDeadPixel,
  toDisplayedFrame,
  DEFAULT_BRIGHTNESS_FACTOR,
  FRAME_BYTES,
} from "../core/display";

test("432 live LEDs, 48 dead cells (led_config.h geometry)", () => {
  assert.equal(LIVE_PIXEL_COUNT, 432);
  assert.equal(DISPLAY_WIDTH * DISPLAY_HEIGHT - LIVE_PIXEL_COUNT, 48);
});

test("dead cells sit only inside the advisory nose cutout", () => {
  for (let y = 0; y < DISPLAY_HEIGHT; y++) {
    for (let x = 0; x < DISPLAY_WIDTH; x++) {
      if (isDeadPixel(x, y)) {
        assert.ok(x >= 15 && x <= 24, `dead cell (${x},${y}) outside cutout x-range`);
        assert.ok(y >= 6, `dead cell (${x},${y}) above cutout`);
      }
    }
  }
});

test("row 6 population matches led_controller.cpp bank rules (17 per bank)", () => {
  // Bank 0 loses x 17..19; bank 1 loses x 20..22.
  assert.equal(isDeadPixel(16, 6), false);
  assert.equal(isDeadPixel(17, 6), true);
  assert.equal(isDeadPixel(19, 6), true);
  assert.equal(isDeadPixel(20, 6), true);
  assert.equal(isDeadPixel(22, 6), true);
  assert.equal(isDeadPixel(23, 6), false);
});

test("brightness truncation reproduces the issue #259 hue drift", () => {
  // Saturated pink (255,0,129) at the default 0.02 factor lands (5,0,2).
  assert.equal(applyBrightness(255, DEFAULT_BRIGHTNESS_FACTOR), 5);
  assert.equal(applyBrightness(0, DEFAULT_BRIGHTNESS_FACTOR), 0);
  assert.equal(applyBrightness(129, DEFAULT_BRIGHTNESS_FACTOR), 2);
  // A "dim" animation drawing at 32/255 is invisible on the panel.
  assert.equal(applyBrightness(32, DEFAULT_BRIGHTNESS_FACTOR), 0);
});

test("toDisplayedFrame masks dead cells even at full brightness", () => {
  const raw = new Uint8Array(FRAME_BYTES).fill(255);
  const out = toDisplayedFrame(raw, 1);
  for (let i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++) {
    const expected = LIVE_MASK[i] === 1 ? 255 : 0;
    assert.equal(out[i * 3], expected);
  }
});
