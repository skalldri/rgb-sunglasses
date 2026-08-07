import { test } from "node:test";
import assert from "node:assert/strict";
import {
  ColorModeResolver,
  animColorFromHue,
  hueLerp,
  sweepPeriodMs,
  timerIntervalMs,
} from "../core/colorMode";

test("animColorFromHue walks the 6-sector wheel with a channel pinned at 255", () => {
  assert.equal(animColorFromHue(0), 0xff0000); // R
  assert.equal(animColorFromHue(128), 0xff8000); // R -> Y midpoint
  assert.equal(animColorFromHue(256), 0xffff00); // Y
  assert.equal(animColorFromHue(512), 0x00ff00); // G
  assert.equal(animColorFromHue(768), 0x00ffff); // C
  assert.equal(animColorFromHue(1024), 0x0000ff); // B
  assert.equal(animColorFromHue(1280), 0xff00ff); // M
  assert.equal(animColorFromHue(1535), 0xff0000); // M->R ramp end == R
  for (let h = 0; h < 1536; h += 7) {
    const c = animColorFromHue(h);
    const channels = [(c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff];
    assert.ok(channels.includes(255), `hue ${h} has no channel at 255`);
  }
});

test("hueLerp takes the shorter arc (through 0 when closer)", () => {
  // 1400 -> 100: forward distance 236 (through 0), backward 1300.
  assert.equal(hueLerp(1400, 100, 0), 1400);
  assert.equal(hueLerp(1400, 100, 256), 100);
  assert.equal(hueLerp(1400, 100, 128), (1400 + 118) % 1536);
  // Plain short arc, no wrap.
  assert.equal(hueLerp(100, 300, 128), 200);
});

test("speed mappings match color_mode_source.cpp", () => {
  assert.equal(sweepPeriodMs(255), 2000);
  assert.equal(sweepPeriodMs(0), 2000 + 255 * 228);
  assert.equal(timerIntervalMs(255), 1000);
  assert.equal(timerIntervalMs(0), 1000 + 255 * 114);
});

test("Static passthrough masks the mode byte; unknown modes act Static", () => {
  const r = new ColorModeResolver(() => 0, () => 0);
  assert.equal(r.resolve(0x00123456), 0x123456);
  // 0xFF = persisted pre-feature default — must render Static.
  assert.equal(r.resolve(0xff123456), 0x123456);
});

test("SpectrumSweep advances phase from dt without hue jumps on speed change", () => {
  let now = 0;
  const r = new ColorModeResolver(() => 0, () => now);
  const raw = (speed: number) => ((0x01 << 24) | (speed << 16)) >>> 0;
  // Reset tick at t=0: phase 0 -> hue 0 -> red.
  assert.equal(r.resolve(raw(255)), 0xff0000);
  // Full period at speed 255 is 2000 ms; half period -> hue 768 -> cyan.
  now = 1000;
  assert.equal(r.resolve(raw(255)), 0x00ffff);
  // Quarter more (500ms = 1/4 period) -> hue 1152 -> B->M midpoint.
  now = 1500;
  assert.equal(r.resolve(raw(255)), animColorFromHue(1152));
});

test("RandomOnActivate rolls once on reset and holds; >=60deg from previous", () => {
  // rng that returns 0 -> roll = (base + 256) % 1536.
  const r = new ColorModeResolver(() => 0, () => 0);
  const raw = (0x03 << 24) >>> 0;
  const first = r.resolve(raw);
  assert.equal(first, animColorFromHue(256)); // from initial hue 0
  assert.equal(r.resolve(raw), first); // holds
  r.notifyActivated();
  assert.equal(r.resolve(raw), animColorFromHue(512)); // rolled again
});

test("RandomOnBeat consumes the latch; without a source degrades to hold", () => {
  let beat = false;
  const r = new ColorModeResolver(() => 0, () => 0, () => {
    const b = beat;
    beat = false;
    return b;
  });
  const raw = (0x02 << 24) >>> 0;
  const initial = r.resolve(raw);
  assert.equal(r.resolve(raw), initial);
  beat = true;
  const next = r.resolve(raw);
  assert.notEqual(next, initial);
  assert.equal(r.resolve(raw), next); // latch consumed, holds again
});

test("RandomTimerFade lerps between picks and rolls at the interval", () => {
  let now = 0;
  const r = new ColorModeResolver(() => 0, () => now);
  const raw = ((0x04 << 24) | (255 << 16)) >>> 0; // 1000 ms interval
  // Reset: currentHue = 256, prev = 256, target = 512.
  assert.equal(r.resolve(raw), animColorFromHue(256));
  now = 500;
  assert.equal(r.resolve(raw), animColorFromHue(hueLerp(256, 512, 128)));
  now = 1000; // rollover: prev=512, target=768, elapsed resets
  assert.equal(r.resolve(raw), animColorFromHue(512));
});
