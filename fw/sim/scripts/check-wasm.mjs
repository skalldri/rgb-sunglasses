#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Stuart Alldritt
// Post-link gate for simulator extension modules — the wasm analog of the
// device's llext symbol resolution (resolve_exports() in extension_host.cpp).
//
//   node check-wasm.mjs <module.wasm>
//
// Asserts:
//  - the module has ZERO imports (the rgbx ABI has no function imports; a
//    nonzero import list means the extension called something the device
//    would also fail to link, e.g. sinf — no libm exists on-target)
//  - all five required rgbx exports are present, plus exported linear memory
import { readFile } from "node:fs/promises";

const path = process.argv[2];
if (!path) {
  console.error("usage: check-wasm.mjs <module.wasm>");
  process.exit(1);
}

const bytes = await readFile(path);
const mod = await WebAssembly.compile(bytes);

const imports = WebAssembly.Module.imports(mod);
if (imports.length > 0) {
  console.error(`${path}: has ${imports.length} import(s) — the device would fail to link these too:`);
  for (const imp of imports) {
    console.error(`  ${imp.kind} ${imp.module}.${imp.name}`);
  }
  process.exit(1);
}

const exportNames = new Set(WebAssembly.Module.exports(mod).map((e) => e.name));
// --dsp: the audio_dsp.wasm module has its own export set.
const required = process.argv.includes("--dsp")
  ? ["memory", "sim_pcm", "sim_band_energy", "sim_beat", "sim_display_bucket", "sim_init", "sim_process"]
  : [
      "memory",
      "rgbx_manifest",
      "rgbx_inputs",
      "rgbx_framebuffer",
      "rgbx_init",
      "rgbx_tick",
    ];
const missing = required.filter((name) => !exportNames.has(name));
if (missing.length > 0) {
  console.error(`${path}: missing required export(s): ${missing.join(", ")}`);
  process.exit(1);
}
