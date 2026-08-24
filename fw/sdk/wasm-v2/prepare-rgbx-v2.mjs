#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Stuart Alldritt

import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";

function fail(message) {
  throw new Error(message);
}

function readU32(bytes, state) {
  let value = 0;
  let shift = 0;
  for (;;) {
    if (state.offset >= bytes.length || shift >= 35) fail("malformed unsigned LEB128");
    const byte = bytes[state.offset++];
    value |= (byte & 0x7f) << shift;
    if ((byte & 0x80) === 0) return value >>> 0;
    shift += 7;
  }
}

function encodeU32(value) {
  const output = [];
  do {
    let byte = value & 0x7f;
    value >>>= 7;
    if (value !== 0) byte |= 0x80;
    output.push(byte);
  } while (value !== 0);
  return Buffer.from(output);
}

function readName(bytes, state) {
  const length = readU32(bytes, state);
  if (length > bytes.length - state.offset) fail("name extends beyond its section");
  const name = bytes.subarray(state.offset, state.offset + length);
  state.offset += length;
  return name;
}

function rewriteExports(payload) {
  const state = { offset: 0 };
  const count = readU32(payload, state);
  const entries = [];
  for (let index = 0; index < count; ++index) {
    const name = readName(payload, state);
    if (state.offset >= payload.length) fail("truncated export kind");
    const kind = payload[state.offset++];
    const itemIndex = readU32(payload, state);
    const text = name.toString("utf8");
    if (kind === 0 && (text === "rgbx_init" || text === "rgbx_tick")) {
      entries.push({ name, kind, itemIndex });
    }
  }
  if (state.offset !== payload.length) fail("trailing bytes in export section");
  if (entries.length !== 2 || entries[0].name.toString("utf8") !== "rgbx_init" ||
      entries[1].name.toString("utf8") !== "rgbx_tick") {
    fail("module must export exactly rgbx_init followed by rgbx_tick");
  }
  const output = [encodeU32(entries.length)];
  for (const entry of entries) {
    output.push(encodeU32(entry.name.length), entry.name, Buffer.from([entry.kind]),
                encodeU32(entry.itemIndex));
  }
  return Buffer.concat(output);
}

function prepare(input) {
  const magic = Buffer.from([0, 97, 115, 109, 1, 0, 0, 0]);
  if (input.length < magic.length || !input.subarray(0, magic.length).equals(magic)) {
    fail("compiler did not emit a WebAssembly v1 module");
  }

  let cursor = magic.length;
  let lastSection = 0;
  let exportSections = 0;
  const output = [magic];
  while (cursor < input.length) {
    const id = input[cursor++];
    const sizeState = { offset: cursor };
    const size = readU32(input, sizeState);
    cursor = sizeState.offset;
    if (size > input.length - cursor) fail("section extends beyond the module");
    let payload = input.subarray(cursor, cursor + size);
    cursor += size;

    if (id === 0) continue; // Strip compiler/linker custom metadata.
    if (id <= lastSection) fail("non-canonical or duplicate standard section order");
    lastSection = id;
    if (id === 4 || id === 5) continue; // Drop unused compiler table/memory declarations.
    if (id === 8) fail("RGBX v2 modules may not define a start function");
    if (id === 9 || id === 11) fail("memoryless RGBX v2 modules may not contain element or data segments");
    if (id === 7) {
      ++exportSections;
      payload = rewriteExports(payload);
    }
    output.push(Buffer.from([id]), encodeU32(payload.length), payload);
  }
  if (exportSections !== 1) fail(`expected one export section, found ${exportSections}`);

  const prepared = Buffer.concat(output);
  // If code actually references the stripped memory/table, validation fails
  // here rather than leaving a latent activation-time failure in the package.
  new WebAssembly.Module(prepared);
  return prepared;
}

const [inputArg, outputArg] = process.argv.slice(2);
if (!inputArg || !outputArg) {
  console.error("usage: prepare-rgbx-v2.mjs <input.raw.wasm> <output.wasm>");
  process.exit(2);
}

const output = prepare(readFileSync(resolve(inputArg)));
writeFileSync(resolve(outputArg), output);
console.log(`prepared memoryless RGBX v2 module: ${output.length} bytes`);
