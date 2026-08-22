#!/usr/bin/env node

import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync, readdirSync } from "node:fs";
import { dirname, join, posix, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const expectedRevision = "8815edc280e6fb039dbdc40dbb4cdebd20d769f5";
const expectedArchiveSha256 = "31be9cfd655879d5c5e9a5067f8e964d70d8ea7e0ea3a38d32c5ace8d163aa92";
const expectedManifestSha256 = "c992f4c8a1a5a1637a0202370136294928b27868bbc6e64f7ce39c3bd6515e80";

const scriptDir = dirname(fileURLToPath(import.meta.url));
const vendorRoot = resolve(scriptDir, "../../third_party/wasm3");
const files = ["LICENSE"];

function walk(relativeDirectory) {
  for (const entry of readdirSync(join(vendorRoot, relativeDirectory), { withFileTypes: true })) {
    const relativePath = posix.join(relativeDirectory, entry.name);
    if (entry.isDirectory()) {
      walk(relativePath);
    } else {
      files.push(relativePath);
    }
  }
}

walk("source");
files.sort();

const manifest = createHash("sha256");
for (const relativePath of files) {
  manifest.update(relativePath);
  manifest.update(Uint8Array.of(0));
  manifest.update(readFileSync(join(vendorRoot, relativePath)));
  manifest.update(Uint8Array.of(0));
}

const record = readFileSync(join(vendorRoot, "VENDORED.md"), "utf8");
assert.ok(record.includes("Revision: `" + expectedRevision + "`"), "vendoring revision drifted");
assert.ok(
  record.includes("Upstream archive SHA-256: `" + expectedArchiveSha256 + "`"),
  "upstream archive digest drifted",
);
assert.ok(
  record.includes("Patched `source/` plus `LICENSE` manifest SHA-256: `" + expectedManifestSha256 + "`"),
  "patched source manifest digest drifted",
);
assert.equal(files.length, 44, "unexpected Wasm3 source/license file count");
assert.equal(manifest.digest("hex"), expectedManifestSha256, "vendored Wasm3 manifest changed");

console.log(`Wasm3 vendor snapshot verified: ${files.length} files, ${expectedManifestSha256}`);
