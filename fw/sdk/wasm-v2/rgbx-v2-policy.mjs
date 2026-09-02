#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Stuart Alldritt
//
// The single place the SDK's RGBX v2 tools learn what the firmware admits.
//
// Every limit here was copied into sdk-manifest.json from <rgbx/rgbx_v2.h> by
// the SDK packager, and the firmware admission path static_asserts its own
// constants against that same header. Neither the post-link gate nor the
// package builder is allowed to carry its own copy of a limit: a missing,
// malformed, or out-of-range manifest field fails closed here rather than
// falling back to a default that could be looser than the device's.
//
// This module owns only the part that needs a filesystem: finding and reading
// the manifest. Validating and expanding the profile it carries lives in
// rgbx-v2-host.mjs next to the host that enforces it, so a consumer with no
// filesystem -- the simulator's browser build -- gets the same expansion
// without pulling node: modules into a bundle. modulePolicy is re-exported
// here so every existing caller keeps one import.

import { readFileSync } from "node:fs";
import { resolve } from "node:path";

import { modulePolicy } from "./rgbx-v2-host.mjs";

export { modulePolicy };

function fail(message) {
  throw new Error(message);
}

/**
 * Load and validate the SDK manifest named by $RGBX_SDK_MANIFEST.
 *
 * The environment variable is mandatory: the SDK's cmake package always sets
 * it, and a tool invoked without it has no way to know which firmware release
 * it is gating for, so guessing would be worse than refusing.
 */
export function loadSdkManifest() {
  const path = process.env.RGBX_SDK_MANIFEST;
  if (!path) {
    fail("RGBX_SDK_MANIFEST is required; invoke this tool through rgbx_add_extension");
  }
  let manifest;
  try {
    manifest = JSON.parse(readFileSync(resolve(path), "utf8"));
  } catch (error) {
    fail(`sdk-manifest at ${path} is unreadable or is not JSON: ${error.message}`);
  }
  if (manifest === null || typeof manifest !== "object" || Array.isArray(manifest)) {
    fail("sdk-manifest must be a JSON object");
  }
  if (!Number.isInteger(manifest.rgbxV2AbiVersion) || manifest.rgbxV2AbiVersion < 1) {
    fail("sdk-manifest rgbxV2AbiVersion must be a positive integer");
  }
  // abiVersion bounds the package's minimumFirmwareAbi. A missing field would
  // make `value > undefined` false in the packager and seal any value, so it is
  // rejected here rather than defaulted.
  if (!Number.isInteger(manifest.abiVersion) || manifest.abiVersion < 1 ||
      manifest.abiVersion > 0xffff) {
    fail("sdk-manifest abiVersion must be an integer in 1..65535");
  }
  if (typeof manifest.wasiSdk !== "string" || manifest.wasiSdk.length === 0) {
    fail("sdk-manifest wasiSdk must name the pinned compiler release");
  }
  return manifest;
}
