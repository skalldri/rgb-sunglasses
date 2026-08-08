#!/usr/bin/env node
// Validate extensions/registry.json — run by community-extensions.yml on
// every registry PR, and runnable locally: node extensions/validate-registry.mjs
//
// Rules (see fw/docs/standalone-extension-repos.md section 7):
//  - unique names matching ^[a-z0-9_]{1,25}$ — the name becomes the .llext
//    filename on /NAND:/ext/ and a release-asset name; the 25-char cap
//    exists because the app's extension sync rejects asset filenames longer
//    than 31 chars INCLUDING the .llext suffix (silently — it just never
//    installs them)
//  - no collision with in-repo extensions (fw/extensions/*/)
//  - https://github.com/ repo URLs
//  - rev is a full 40-hex commit SHA (a pinned commit, never a branch/tag:
//    post-review tampering with the source repo must be inert)
//  - license is required (OSI-approved expected; reviewed by a human)
import { readFile, readdir } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const registryPath = join(here, "registry.json");

const errors = [];
let registry;
try {
  registry = JSON.parse(await readFile(registryPath, "utf8"));
} catch (e) {
  console.error(`registry.json: unparseable: ${e.message}`);
  process.exit(1);
}

if (registry.version !== 1) {
  errors.push(`unknown registry version ${registry.version} (expected 1)`);
}
if (!Array.isArray(registry.extensions)) {
  errors.push("'extensions' must be an array");
  console.error(errors.join("\n"));
  process.exit(1);
}

// In-repo extensions own their names (they ship as release assets from the
// firmware build; a registry entry with the same name would collide).
const inRepo = new Set();
for (const entry of await readdir(join(here, "../fw/extensions"), { withFileTypes: true })) {
  if (entry.isDirectory()) inRepo.add(entry.name);
}

const seen = new Set();
for (const [i, ext] of registry.extensions.entries()) {
  const where = `extensions[${i}]${ext?.name ? ` (${ext.name})` : ""}`;
  for (const field of ["name", "repo", "rev", "description", "author", "license"]) {
    if (typeof ext?.[field] !== "string" || ext[field].length === 0) {
      errors.push(`${where}: missing or empty '${field}'`);
    }
  }
  if (typeof ext?.name === "string") {
    if (!/^[a-z0-9_]{1,25}$/.test(ext.name)) {
      errors.push(`${where}: name must match ^[a-z0-9_]{1,25}$ (it becomes the .llext filename; >25 chars is silently skipped by app-side sync)`);
    }
    if (seen.has(ext.name)) {
      errors.push(`${where}: duplicate name '${ext.name}'`);
    }
    seen.add(ext.name);
    if (inRepo.has(ext.name)) {
      errors.push(`${where}: name collides with in-repo extension fw/extensions/${ext.name}/`);
    }
  }
  // Charset-tight on purpose: registry values are interpolated into CI
  // context, so this is an injection surface, not just a format check.
  // GitHub owner names are alphanumeric+hyphen; repo names add . and _.
  if (typeof ext?.repo === "string" && !/^https:\/\/github\.com\/[A-Za-z0-9-]+\/[A-Za-z0-9._-]+$/.test(ext.repo)) {
    errors.push(`${where}: repo must be https://github.com/<owner>/<name> with only [A-Za-z0-9._-] segments`);
  }
  if (typeof ext?.rev === "string" && !/^[0-9a-f]{40}$/.test(ext.rev)) {
    errors.push(`${where}: rev must be a full 40-hex commit SHA (never a branch or tag)`);
  }
}

if (errors.length > 0) {
  console.error(errors.join("\n"));
  process.exit(1);
}
console.log(`registry.json OK (${registry.extensions.length} extension(s))`);
