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
 */

import fs from "node:fs";
import path from "node:path";
import { defineConfig, type Plugin } from "vite";
import basicSsl from "@vitejs/plugin-basic-ssl";

const simDir = __dirname;
const wasmDir = path.resolve(simDir, "out/wasm");

/** Serves a JSON array of the built .wasm modules: [{name, url, size}]. */
function wasmIndexPlugin(): Plugin {
  return {
    name: "rgbx-wasm-index",
    configureServer(server) {
      server.middlewares.use("/wasm-index.json", (_req, res) => {
        let entries: { name: string; url: string; size: number }[] = [];
        try {
          entries = fs
            .readdirSync(wasmDir)
            .filter((f) => f.endsWith(".wasm"))
            .sort()
            .map((name) => ({
              name,
              url: `/wasm/${name}`,
              size: fs.statSync(path.join(wasmDir, name)).size,
            }));
        } catch {
          // out/wasm missing = extensions not built yet; an empty index is a
          // valid answer, and the UI says so rather than failing to load.
        }
        res.setHeader("Content-Type", "application/json");
        res.setHeader("Cache-Control", "no-store");
        res.end(JSON.stringify(entries));
      });
    },
  };
}

export default defineConfig({
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
});
