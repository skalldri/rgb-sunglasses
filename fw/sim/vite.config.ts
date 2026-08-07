/**
 * Dev-server config for the browser simulator UI (fw/sim/browser).
 *
 * Two things this has to arrange that a stock Vite setup does not:
 *  - the built wasm artifacts live OUTSIDE the Vite root (fw/sim/out/wasm,
 *    produced by build-extensions.sh), so `out/` is mounted as publicDir —
 *    out/wasm/hello.wasm is served as GET /wasm/hello.wasm;
 *  - the UI imports the platform-agnostic core from ../core, which is also
 *    outside the root, so server.fs.allow has to reach fw/sim.
 *
 * The extension picker needs to enumerate what was actually built, and a
 * static publicDir has no directory listing — hence the tiny middleware
 * below serving /wasm-index.json.
 *
 * This config targets the dev server (`npm run serve`). A production
 * `vite build` is wired up but is not part of any gate.
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
        url: `/wasm/${name}`,
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

const config: UserConfig = {
  root: path.resolve(simDir, "browser"),
  publicDir: path.resolve(simDir, "out"),
  // basic-ssl is opt-in: the mic and DeviceMotion need a secure context, and
  // http://localhost only counts as one for the machine running the browser.
  // Set RGBX_SIM_SSL=1 to reach the sim from a phone over the LAN by IP.
  plugins: [wasmIndexPlugin(), ...(process.env.RGBX_SIM_SSL === "1" ? [basicSsl()] : [])],
  server: {
    fs: { allow: [simDir] },
  },
  build: {
    outDir: path.resolve(simDir, "dist/browser"),
    emptyOutDir: true,
  },
};

export default config;
