#!/usr/bin/env node
// Parity check between an SDK-built and a build-extensions.sh-built wasm
// module: the export and import SETS must match exactly. (Byte-compare is
// deliberately not the standard — -g embeds build paths, so byte identity
// across build dirs is fragile by construction; the sets are what the
// simulator's load contract actually consumes.)
//
//   node compare-wasm-exports.mjs <a.wasm> <b.wasm>
import { readFile } from "node:fs/promises";

const [a, b] = process.argv.slice(2);
if (!a || !b) {
  console.error("usage: compare-wasm-exports.mjs <a.wasm> <b.wasm>");
  process.exit(2);
}

async function sets(path) {
  const mod = await WebAssembly.compile(await readFile(path));
  return {
    exports: WebAssembly.Module.exports(mod)
      .map((e) => `${e.kind} ${e.name}`)
      .sort(),
    imports: WebAssembly.Module.imports(mod)
      .map((i) => `${i.kind} ${i.module}.${i.name}`)
      .sort(),
  };
}

const [sa, sb] = await Promise.all([sets(a), sets(b)]);
let failed = false;
for (const kind of ["exports", "imports"]) {
  const onlyA = sa[kind].filter((x) => !sb[kind].includes(x));
  const onlyB = sb[kind].filter((x) => !sa[kind].includes(x));
  if (onlyA.length || onlyB.length) {
    failed = true;
    console.error(`${kind} differ:`);
    for (const x of onlyA) console.error(`  only in ${a}: ${x}`);
    for (const x of onlyB) console.error(`  only in ${b}: ${x}`);
  }
}
if (failed) process.exit(1);
console.log(`${a} and ${b}: export/import sets match (${sa.exports.length} exports, ${sa.imports.length} imports)`);
