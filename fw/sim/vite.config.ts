/**
 * Dev-server config for the browser simulator UI (fw/sim/browser).
 *
 * Two things this has to arrange that a stock Vite setup does not:
 *  - the built wasm artifacts live OUTSIDE the Vite root (fw/sim/out/wasm,
 *    produced by build-extensions.sh), so out/wasm is mounted as publicDir —
 *    modules serve FLAT at <base><name>.wasm (e.g. GET /hello.wasm in dev,
 *    /sim/hello.wasm on Pages);
 *  - the UI imports the platform-agnostic core from ../core, which is also
 *    outside the root, so server.fs.allow has to reach fw/sim.
 *
 * The extension picker needs to enumerate what was actually built, and a
 * static publicDir has no directory listing — hence the tiny middleware
 * below serving /wasm-index.json.
 *
 * Serves the dev loop (`npm run serve`) AND the Pages deployment: the
 * production `vite build` (base=/sim/) is composed into site/sim/ by
 * pages.yml and gated pre-merge by sim-ci.yml's production-bundle step.
 *
 * Launch it as `vite` from fw/sim, NOT `vite browser` — Vite looks for its
 * config inside the root given on the command line, so passing `browser` as
 * a positional makes it silently ignore this file (and then the wasm index
 * 404s into the SPA fallback). `root` below is what selects browser/.
 */

import fs from "node:fs";
import path from "node:path";
import type { Plugin, UserConfig } from "vite";
import basicSsl from "@vitejs/plugin-basic-ssl";

const simDir = __dirname;
const wasmDir = path.resolve(simDir, "out/wasm");
const scenariosDir = path.resolve(simDir, "scenarios");

interface WasmEntry {
  name: string;
  url: string;
  size: number;
}

/** Enumerates out/wasm. An unreadable directory (extensions not built yet)
 * is an empty index, not an error — the UI says so and stays usable. */
function readWasmIndex(): WasmEntry[] {
  try {
    return fs
      .readdirSync(wasmDir)
      .filter((f) => f.endsWith(".wasm"))
      .sort()
      .map((name) => ({
        name,
        // RELATIVE to the deploy base — the client resolves it against
        // import.meta.env.BASE_URL, so the same index works at "/" (dev)
        // and "/sim/" (Pages).
        url: name,
        size: fs.statSync(path.join(wasmDir, name)).size,
      }));
  } catch {
    return [];
  }
}

/** Serves /wasm-index.json — read fresh per request in dev so rebuilding
 * extensions needs no server restart, and frozen into the bundle on build. */
function wasmIndexPlugin(): Plugin {
  return {
    name: "rgbx-wasm-index",
    configureServer(server) {
      server.middlewares.use("/wasm-index.json", (_req, res) => {
        res.setHeader("Content-Type", "application/json");
        res.setHeader("Cache-Control", "no-store");
        res.end(JSON.stringify(readWasmIndex()));
      });
    },
    generateBundle() {
      this.emitFile({
        type: "asset",
        fileName: "wasm-index.json",
        source: JSON.stringify(readWasmIndex()),
      });
    },
  };
}

interface ScenarioEntry {
  name: string;
  url: string;
  description: string;
  durationMs: number;
}

/** Enumerates scenarios/*.json. Same contract as the wasm index: a missing
 * directory is an empty index, unparseable files are skipped with a warning
 * (the CLI would reject them too), and urls are RELATIVE to the deploy base. */
function readScenarioIndex(): ScenarioEntry[] {
  let files: string[];
  try {
    files = fs.readdirSync(scenariosDir).filter((f) => f.endsWith(".json"));
  } catch {
    return [];
  }
  const entries: ScenarioEntry[] = [];
  for (const file of files.sort()) {
    try {
      const s = JSON.parse(fs.readFileSync(path.join(scenariosDir, file), "utf8"));
      if (s?.schema !== "rgbx-scenario/1" || typeof s.name !== "string") {
        throw new Error("not an rgbx-scenario/1 file");
      }
      entries.push({
        name: s.name,
        url: `scenarios/${file}`,
        description: typeof s.description === "string" ? s.description : "",
        durationMs: typeof s.durationMs === "number" ? s.durationMs : 0,
      });
    } catch (err) {
      console.warn(`scenario-index: skipping ${file}: ${String(err)}`);
    }
  }
  return entries;
}

/** Every file the scenario player may fetch: the scenario JSONs plus their
 * `file:` assets (scenarios/assets/... — WAV captures, D-line dumps). */
function listScenarioFiles(): string[] {
  const out: string[] = [];
  const walk = (rel: string): void => {
    let names: fs.Dirent[];
    try {
      names = fs.readdirSync(path.join(scenariosDir, rel), { withFileTypes: true });
    } catch {
      return;
    }
    for (const d of names) {
      const relPath = rel === "" ? d.name : `${rel}/${d.name}`;
      if (d.isDirectory()) {
        walk(relPath);
      } else if (d.isFile()) {
        out.push(relPath);
      }
    }
  };
  walk("");
  return out.sort();
}

const CONTENT_TYPES: Record<string, string> = {
  ".json": "application/json",
  ".wav": "audio/wav",
  ".txt": "text/plain",
};

/** Serves /scenario-index.json and /scenarios/* — same split as the wasm
 * plugin: read fresh per request in dev (a new capture_to_scenario.py output
 * shows up without a server restart), frozen into the bundle on build. */
function scenarioIndexPlugin(): Plugin {
  return {
    name: "rgbx-scenario-index",
    configureServer(server) {
      server.middlewares.use("/scenario-index.json", (_req, res) => {
        res.setHeader("Content-Type", "application/json");
        res.setHeader("Cache-Control", "no-store");
        res.end(JSON.stringify(readScenarioIndex()));
      });
      server.middlewares.use("/scenarios", (req, res, next) => {
        // connect strips the mount prefix; what's left is the file path.
        const rel = decodeURIComponent((req.url ?? "").split("?")[0]).replace(/^\/+/, "");
        const resolved = path.resolve(scenariosDir, rel);
        // Path-traversal guard: the resolved file must stay under scenarios/.
        if (rel === "" || !resolved.startsWith(scenariosDir + path.sep)) {
          next();
          return;
        }
        let bytes: Buffer;
        try {
          bytes = fs.readFileSync(resolved);
        } catch {
          next();
          return;
        }
        res.setHeader(
          "Content-Type",
          CONTENT_TYPES[path.extname(resolved)] ?? "application/octet-stream",
        );
        res.setHeader("Cache-Control", "no-store");
        res.end(bytes);
      });
    },
    generateBundle() {
      this.emitFile({
        type: "asset",
        fileName: "scenario-index.json",
        source: JSON.stringify(readScenarioIndex()),
      });
      for (const rel of listScenarioFiles()) {
        this.emitFile({
          type: "asset",
          fileName: `scenarios/${rel}`,
          source: fs.readFileSync(path.join(scenariosDir, rel)),
        });
      }
    },
  };
}

const config: UserConfig = {
  root: path.resolve(simDir, "browser"),
  // Deploy base: "/" for the dev server, "/sim/" when the Pages workflow
  // builds the copy hosted at rgb-sunglasses.autom8ed.com/sim/. Everything
  // that references bundled assets goes through import.meta.env.BASE_URL
  // (or Vite's own rewriting), so no code changes per deployment target.
  base: process.env.RGBX_SIM_BASE ?? "/",
  // ONLY the wasm modules — out/ itself also holds CLI artifacts (runs/,
  // *-obj/) that must never ship in a deployed bundle. Modules therefore
  // serve at <base><name>.wasm (flat), matching the index urls below.
  publicDir: wasmDir,
  // basic-ssl is opt-in: the mic and DeviceMotion need a secure context, and
  // http://localhost only counts as one for the machine running the browser.
  // Set RGBX_SIM_SSL=1 to reach the sim from a phone over the LAN by IP.
  plugins: [
    wasmIndexPlugin(),
    scenarioIndexPlugin(),
    ...(process.env.RGBX_SIM_SSL === "1" ? [basicSsl()] : []),
  ],
  server: {
    fs: { allow: [simDir] },
  },
  build: {
    outDir: path.resolve(simDir, "dist/browser"),
    emptyOutDir: true,
  },
};

export default config;
