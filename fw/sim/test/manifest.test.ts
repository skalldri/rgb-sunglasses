import { test } from "node:test";
import assert from "node:assert/strict";
import { MANIFEST, PARAM_DESC, RgbxParamType } from "../core/abi";
import { ManifestResult, validateManifest } from "../core/manifest";

/** Builds a synthetic linear memory holding a manifest at a fixed address,
 * mirroring what the extension image would contain. */
function buildMemory(opts: {
  abiVersion?: number;
  width?: number;
  height?: number;
  name?: string | null | "unterminated";
  params?: { name?: string | null; type: number; defaultValue?: number; defaultStr?: string }[];
  paramsPtrOverride?: number;
  paramCountOverride?: number;
}): { memory: ArrayBuffer; manifestAddr: number } {
  const memory = new ArrayBuffer(4096);
  const view = new DataView(memory);
  const u8 = new Uint8Array(memory);
  const enc = new TextEncoder();

  let heap = 1024; // string/table storage
  const alloc = (bytes: Uint8Array): number => {
    const at = heap;
    u8.set(bytes, at);
    heap += bytes.length;
    return at;
  };
  const allocString = (s: string, terminated = true): number =>
    alloc(enc.encode(terminated ? s + "\0" : s));

  const manifestAddr = 64;
  view.setUint32(manifestAddr + MANIFEST.abiVersion, opts.abiVersion ?? 1, true);
  const name = opts.name;
  let namePtr = 0;
  if (name === "unterminated") {
    // 300 non-NUL bytes: exceeds the 256-byte scan bound.
    namePtr = alloc(new Uint8Array(300).fill(65));
  } else if (typeof name === "string") {
    namePtr = allocString(name);
  } else if (name === undefined) {
    namePtr = allocString("Test");
  }
  view.setUint32(manifestAddr + MANIFEST.name, namePtr, true);
  view.setUint32(manifestAddr + MANIFEST.width, opts.width ?? 40, true);
  view.setUint32(manifestAddr + MANIFEST.height, opts.height ?? 12, true);

  const params = opts.params ?? [];
  let paramsPtr = 0;
  if (params.length > 0) {
    const table = new Uint8Array(params.length * PARAM_DESC.size);
    paramsPtr = alloc(table);
    params.forEach((p, i) => {
      const at = paramsPtr + i * PARAM_DESC.size;
      const pName = p.name === null ? 0 : allocString(p.name ?? `P${i}`);
      view.setUint32(at + PARAM_DESC.name, pName, true);
      view.setUint32(at + PARAM_DESC.type, p.type, true);
      const def =
        p.defaultStr !== undefined ? allocString(p.defaultStr) : (p.defaultValue ?? 0);
      view.setUint32(at + PARAM_DESC.defaultValue, def, true);
    });
  }
  view.setUint32(
    manifestAddr + MANIFEST.paramCount,
    opts.paramCountOverride ?? params.length,
    true,
  );
  view.setUint32(manifestAddr + MANIFEST.params, opts.paramsPtrOverride ?? paramsPtr, true);

  return { memory, manifestAddr };
}

const ENV = { expectedWidth: 40, expectedHeight: 12 };

test("accepts a well-formed manifest and slots string params", () => {
  const { memory, manifestAddr } = buildMemory({
    name: "Kitchen Sink",
    params: [
      { name: "Speed", type: RgbxParamType.Uint32, defaultValue: 50 },
      { name: "Msg", type: RgbxParamType.String, defaultStr: "HELLO" },
      { name: "On", type: RgbxParamType.Bool, defaultValue: 7 }, // clamps to 1
      { name: "Msg2", type: RgbxParamType.String, defaultStr: "" },
    ],
  });
  const out = validateManifest(memory, manifestAddr, ENV);
  assert.equal(out.result, ManifestResult.Ok);
  const meta = out.metadata!;
  assert.equal(meta.displayName, "Kitchen Sink");
  assert.equal(meta.paramCount, 4);
  assert.equal(meta.stringParamCount, 2);
  assert.equal(meta.params[0].stringSlot, 0xff);
  assert.equal(meta.params[1].stringSlot, 0); // 1st string param -> slot 0
  assert.equal(meta.params[2].defaultValue, 1); // BOOL clamped
  assert.equal(meta.params[3].stringSlot, 1); // 2nd string param -> slot 1
  assert.equal(meta.stringDefaults[0], "HELLO");
});

test("BadManifestPointer before any field read", () => {
  const { memory } = buildMemory({});
  assert.equal(validateManifest(memory, 0, ENV).result, ManifestResult.BadManifestPointer);
  assert.equal(
    validateManifest(memory, 4096 - 4, ENV).result,
    ManifestResult.BadManifestPointer,
  );
});

test("BadAbiVersion / BadDims", () => {
  const v = buildMemory({ abiVersion: 2 });
  assert.equal(validateManifest(v.memory, v.manifestAddr, ENV).result, ManifestResult.BadAbiVersion);
  const d = buildMemory({ width: 20 });
  assert.equal(validateManifest(d.memory, d.manifestAddr, ENV).result, ManifestResult.BadDims);
});

test("BadParamTable: count/pointer inconsistency both ways", () => {
  const a = buildMemory({ paramCountOverride: 1 }); // count=1, params NULL
  assert.equal(validateManifest(a.memory, a.manifestAddr, ENV).result, ManifestResult.BadParamTable);
  const b = buildMemory({ paramsPtrOverride: 2048 }); // count=0, params set
  assert.equal(validateManifest(b.memory, b.manifestAddr, ENV).result, ManifestResult.BadParamTable);
  const c = buildMemory({ paramCountOverride: 17, paramsPtrOverride: 1024 });
  assert.equal(validateManifest(c.memory, c.manifestAddr, ENV).result, ManifestResult.BadParamTable);
});

test("nameless manifest -> 'unnamed'; nameless param -> 'param'", () => {
  const { memory, manifestAddr } = buildMemory({
    name: null,
    params: [{ name: null, type: RgbxParamType.Uint32 }],
  });
  const out = validateManifest(memory, manifestAddr, ENV);
  assert.equal(out.result, ManifestResult.Ok);
  assert.equal(out.metadata!.displayName, "unnamed");
  assert.equal(out.metadata!.params[0].name, "param");
});

test("BadName on a string with no NUL within the 256-byte scan bound", () => {
  const { memory, manifestAddr } = buildMemory({ name: "unterminated" });
  assert.equal(validateManifest(memory, manifestAddr, ENV).result, ManifestResult.BadName);
});

test("long display name truncates to 23 bytes (kMaxNameLen 24)", () => {
  const long = "A".repeat(100);
  const { memory, manifestAddr } = buildMemory({ name: long });
  const out = validateManifest(memory, manifestAddr, ENV);
  assert.equal(out.result, ManifestResult.Ok);
  assert.equal(out.metadata!.displayName, "A".repeat(23));
});

test("BadParamType on out-of-enum type", () => {
  const { memory, manifestAddr } = buildMemory({ params: [{ type: 9 }] });
  assert.equal(validateManifest(memory, manifestAddr, ENV).result, ManifestResult.BadParamType);
});

test("TooManyStringParams at the fifth string param", () => {
  const { memory, manifestAddr } = buildMemory({
    params: Array.from({ length: 5 }, () => ({ type: RgbxParamType.String, defaultStr: "x" })),
  });
  assert.equal(
    validateManifest(memory, manifestAddr, ENV).result,
    ManifestResult.TooManyStringParams,
  );
});

test("BadStringDefault: overlong defaults rejected, not truncated (32+ bytes)", () => {
  const { memory, manifestAddr } = buildMemory({
    params: [{ type: RgbxParamType.String, defaultStr: "B".repeat(32) }],
  });
  assert.equal(
    validateManifest(memory, manifestAddr, ENV).result,
    ManifestResult.BadStringDefault,
  );
  // 31 bytes is the longest legal default.
  const ok = buildMemory({ params: [{ type: RgbxParamType.String, defaultStr: "B".repeat(31) }] });
  assert.equal(validateManifest(ok.memory, ok.manifestAddr, ENV).result, ManifestResult.Ok);
});
