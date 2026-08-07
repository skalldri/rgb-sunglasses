import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as path from "node:path";
import { rejectionMessage, sniffModuleKind } from "../core/moduleSniff";

test("recognizes the wasm magic", () => {
  assert.equal(sniffModuleKind(new Uint8Array([0x00, 0x61, 0x73, 0x6d, 1, 0, 0, 0])), "wasm");
});

test("recognizes an ELF (.llext) and names the device/sim split", () => {
  assert.equal(sniffModuleKind(new Uint8Array([0x7f, 0x45, 0x4c, 0x46, 1, 1, 1, 0])), "elf");
  const msg = rejectionMessage("plasma.llext", "elf");
  assert.match(msg, /\.llext/);
  assert.match(msg, /build-extensions\.sh/);
});

test("everything else is unknown, including short buffers", () => {
  assert.equal(sniffModuleKind(new Uint8Array([])), "unknown");
  assert.equal(sniffModuleKind(new Uint8Array([0x00])), "unknown");
  assert.equal(sniffModuleKind(new Uint8Array([0x7f, 0x45])), "unknown");
  assert.equal(sniffModuleKind(new TextEncoder().encode("hello world")), "unknown");
  assert.match(rejectionMessage("notes.txt", "unknown"), /not a WebAssembly module/);
});

test("a real built module sniffs as wasm", (t) => {
  const p = path.join(__dirname, "..", "..", "out", "wasm", "hello.wasm");
  if (!fs.existsSync(p)) {
    t.skip("hello.wasm not built");
    return;
  }
  assert.equal(sniffModuleKind(new Uint8Array(fs.readFileSync(p))), "wasm");
});
