import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const headerUrl = new URL("../../src/animations/wasm_mvp_module.h", import.meta.url);
const header = readFileSync(headerUrl, "utf8");
const initializer = header.match(/kWasmMvpModule\[\]\s*=\s*\{([\s\S]*?)\};/);
assert(initializer, "could not find kWasmMvpModule initializer");

const bytes = Uint8Array.from(
  [...initializer[1].matchAll(/0x([0-9a-f]{2})/gi)],
  (match) => Number.parseInt(match[1], 16),
);
assert.equal(bytes.length, 85, "unexpected embedded module size");

const module = new WebAssembly.Module(bytes);
assert.deepEqual(WebAssembly.Module.imports(module), [
  { module: "rgbx_mvp", name: "fill", kind: "function" },
]);
assert.deepEqual(WebAssembly.Module.exports(module), [
  { name: "rgbx_tick", kind: "function" },
]);

const colors = [];
const instance = new WebAssembly.Instance(module, {
  rgbx_mvp: { fill: (color) => colors.push(color >>> 0) },
});
for (const elapsedMs of [0, 499, 500, 999, 1000]) {
  instance.exports.rgbx_tick(elapsedMs);
}
assert.deepEqual(colors, [0x00ffff, 0x00ffff, 0xff00ff, 0xff00ff, 0x00ffff]);

assert.throws(
  () => new WebAssembly.Instance(module, { rgbx_mvp: {} }),
  WebAssembly.LinkError,
);

const corrupted = bytes.slice();
corrupted[0] ^= 0xff;
assert.throws(() => new WebAssembly.Module(corrupted), WebAssembly.CompileError);

console.log("Wasm MVP module contract passed");
