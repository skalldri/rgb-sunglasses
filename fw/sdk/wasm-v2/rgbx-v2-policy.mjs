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

import { readFileSync } from "node:fs";
import { resolve } from "node:path";

// field -> [minimum, maximum] sanity window. These are not a second policy:
// they only reject a manifest that is corrupt or from an incompatible SDK, so
// a damaged pin cannot silently widen what the tools accept.
const REQUIRED_POLICY_FIELDS = {
  width: [1, 4096],
  height: [1, 4096],
  pixelsPerSpan: [1, 64],
  maxParams: [1, 256],
  maxStringParams: [0, 256],
  stringParamSize: [1, 4096],
  audioBandCount: [1, 256],
  audioDisplayBucketCount: [1, 256],
  imuAxisCount: [1, 16],
  maxDiagnosticsPerTick: [0, 256],
  moduleMaxBytes: [8, 1 << 20],
  maxFunctions: [1, 1024],
  maxGlobals: [0, 1024],
  maxLocalsPerFunction: [0, 1024],
  minImports: [0, 64],
  maxImports: [0, 64],
  maxParamCallsPerTick: [0, 4096],
  maxInputCallsPerTick: [0, 4096],
  sectionAllowedMask: [1, 0xffffffff],
  sectionRequiredMask: [1, 0xffffffff],
};

function fail(message) {
  throw new Error(message);
}

function requireInteger(value, name, [minimum, maximum]) {
  if (!Number.isInteger(value) || value < minimum || value > maximum) {
    fail(`sdk-manifest ${name} must be an integer in ${minimum}..${maximum}`);
  }
  return value;
}

function maskToSet(mask, name) {
  const ids = new Set();
  for (let id = 0; id < 32; ++id) {
    if ((mask & (1 << id)) !== 0) ids.add(id);
  }
  if (ids.size === 0) fail(`sdk-manifest ${name} selects no section ids`);
  return ids;
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

/**
 * Validate and expand the RGBX v2 admission profile carried by a manifest.
 *
 * Returns the pinned fields plus the values every consumer derives the same
 * way, so no caller re-derives a frame geometry of its own.
 */
export function modulePolicy(manifest) {
  const pinned = manifest.rgbxV2ModulePolicy;
  if (pinned === null || typeof pinned !== "object" || Array.isArray(pinned)) {
    fail("sdk-manifest rgbxV2ModulePolicy is missing; this SDK predates the pinned v2 profile");
  }
  const policy = {};
  for (const [name, range] of Object.entries(REQUIRED_POLICY_FIELDS)) {
    policy[name] = requireInteger(pinned[name], `rgbxV2ModulePolicy.${name}`, range);
  }
  if (policy.minImports > policy.maxImports) {
    fail("sdk-manifest rgbxV2ModulePolicy.minImports exceeds maxImports");
  }
  if ((policy.sectionRequiredMask & ~policy.sectionAllowedMask) !== 0) {
    fail("sdk-manifest rgbxV2ModulePolicy requires a section id it does not admit");
  }

  const capabilityBits = pinned.capabilityBits;
  if (capabilityBits === null || typeof capabilityBits !== "object" ||
      Array.isArray(capabilityBits)) {
    fail("sdk-manifest rgbxV2ModulePolicy.capabilityBits must be an object");
  }
  for (const [name, bit] of Object.entries(capabilityBits)) {
    if (!/^[a-z][a-z0-9_]*$/.test(name) || !Number.isInteger(bit) || bit <= 0 ||
        (bit & (bit - 1)) !== 0) {
      fail(`sdk-manifest capability ${name} must map to a single power-of-two bit`);
    }
  }

  const pixelCount = policy.width * policy.height;
  if (pixelCount % policy.pixelsPerSpan !== 0) {
    fail("sdk-manifest frame geometry is not an exact multiple of the span width");
  }
  return {
    ...policy,
    capabilityBits,
    pixelCount,
    spanCallsPerTick: pixelCount / policy.pixelsPerSpan,
    allowedSections: maskToSet(policy.sectionAllowedMask, "sectionAllowedMask"),
    requiredSections: maskToSet(policy.sectionRequiredMask, "sectionRequiredMask"),
  };
}
