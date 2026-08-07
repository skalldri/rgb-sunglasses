/**
 * Dependency-free PNG encoder (node:zlib deflate) + glasses-frame
 * rendering: x10 nearest-neighbor upscale, dead cells painted as dark
 * bezel so the nose cutout is visible in the image an agent Reads.
 */

import { deflateSync } from "node:zlib";
import { DISPLAY_HEIGHT, DISPLAY_WIDTH, LIVE_MASK } from "../core/display";

const CRC_TABLE: Uint32Array = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    table[n] = c >>> 0;
  }
  return table;
})();

function crc32(bytes: Uint8Array): number {
  let crc = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) {
    crc = CRC_TABLE[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function chunk(type: string, data: Uint8Array): Uint8Array {
  const out = new Uint8Array(12 + data.length);
  const view = new DataView(out.buffer);
  view.setUint32(0, data.length);
  out.set([...type].map((c) => c.charCodeAt(0)), 4);
  out.set(data, 8);
  view.setUint32(8 + data.length, crc32(out.subarray(4, 8 + data.length)));
  return out;
}

/** Encodes RGB8 pixels (row-major, 3 bytes/px) to a PNG buffer. */
export function encodePng(width: number, height: number, rgb: Uint8Array): Buffer {
  const ihdr = new Uint8Array(13);
  const ihdrView = new DataView(ihdr.buffer);
  ihdrView.setUint32(0, width);
  ihdrView.setUint32(4, height);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 2; // color type: truecolor
  // filter 0 per scanline
  const raw = new Uint8Array(height * (1 + width * 3));
  for (let y = 0; y < height; y++) {
    raw.set(rgb.subarray(y * width * 3, (y + 1) * width * 3), y * (1 + width * 3) + 1);
  }
  const idat = deflateSync(raw);
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr),
    chunk("IDAT", new Uint8Array(idat)),
    chunk("IEND", new Uint8Array(0)),
  ]);
}

const SCALE = 10;
const BEZEL: [number, number, number] = [24, 24, 28];
const GAP: [number, number, number] = [10, 10, 12];

/** Renders a (possibly brightness-scaled) 40x12 frame as a PNG with the
 * glasses shape visible: LEDs as filled squares with a 1px gap, dead
 * cells as flat bezel. */
export function frameToPng(frame: Uint8Array): Buffer {
  const w = DISPLAY_WIDTH * SCALE;
  const h = DISPLAY_HEIGHT * SCALE;
  const rgb = new Uint8Array(w * h * 3);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const cx = Math.trunc(x / SCALE);
      const cy = Math.trunc(y / SCALE);
      const cell = cy * DISPLAY_WIDTH + cx;
      let color: [number, number, number];
      if (LIVE_MASK[cell] === 0) {
        color = BEZEL;
      } else if (x % SCALE === 0 || y % SCALE === 0) {
        color = GAP; // 1px grid gap so individual LEDs read as LEDs
      } else {
        color = [frame[cell * 3], frame[cell * 3 + 1], frame[cell * 3 + 2]];
      }
      const o = (y * w + x) * 3;
      rgb[o] = color[0];
      rgb[o + 1] = color[1];
      rgb[o + 2] = color[2];
    }
  }
  return encodePng(w, h, rgb);
}
