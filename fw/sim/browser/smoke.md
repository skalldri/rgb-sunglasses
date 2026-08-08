# Browser simulator — launching and smoke-testing

The browser UI runs an rgbx extension against the same platform-agnostic
simulation core the Node CLI uses (`fw/sim/core/`), with a Web Worker
standing in for the device's K_USER sandbox thread. No hardware is involved
at any point — nothing here needs the `board` lock.

## 1. Build the wasm artifacts first

The extension picker enumerates `fw/sim/out/wasm/*.wasm`, and the audio
sources need `audio_dsp.wasm` from the same directory:

```bash
fw/sim/build-extensions.sh          # no hardware required
ls fw/sim/out/wasm                  # hello.wasm  cpptest.wasm  audio_dsp.wasm
```

If `out/wasm` is missing the page still loads, the picker is empty, and the
console panel says exactly that.

## 2. Launch

```bash
cd fw/sim && npm run serve          # -> http://localhost:5173
```

`npm run serve` is `vite --host`. **Do not add a `browser` positional**
(`vite browser`): Vite resolves its config file relative to the root given on
the command line, so that makes it silently skip `fw/sim/vite.config.ts` —
`/wasm-index.json` and the flat `/<name>.wasm` module routes then fall through to the SPA fallback
and return `index.html` with a 200. The config sets `root` itself.

If the Node CLI's `fw/sim/rgbx-sim` grows a `serve` subcommand it delegates to
the same Vite dev server; either entry point is fine.

## 3. Smoke test without a browser

Everything below is checkable from a shell — useful in CI or when no display
is available:

```bash
cd fw/sim
npx vite --port 5199 --strictPort &   # or: npm run serve -- --port 5199

curl -s -o /dev/null -w '%{http_code}\n'            http://localhost:5199/            # 200, text/html
curl -s                                            http://localhost:5199/wasm-index.json
curl -s -o /dev/null -w '%{http_code} %{content_type}\n' http://localhost:5199/hello.wasm  # 200 application/wasm (modules serve FLAT — publicDir is out/wasm)
curl -s -o /dev/null -w '%{http_code}\n'            http://localhost:5199/main.ts     # 200, transformed TS
```

A non-200 (or an `index.html` body) from `/wasm-index.json` means the config
was not loaded — see the positional-argument warning above.

Type checking is the other half of the offline gate:

```bash
cd fw/sim && npx tsc -p tsconfig.browser.json --noEmit
```

## 4. Driving it from the phone

Serving the UI to the phone's Chrome is the way to exercise DeviceMotion and
the phone's microphone. Two routes:

**adb reverse (preferred, USB, no TLS needed).** `http://localhost:5173` on
the phone counts as a secure origin, so the mic and motion sensors work
without certificates.

```bash
adb reverse tcp:5173 tcp:5173
# then open http://localhost:5173 in Chrome on the phone
```

> Any `adb` command needs the `app` hardware lock held first — see the
> "Hardware locking" section of the root `CLAUDE.md`
> (`scripts/hw-lock.sh hold app` via a persistent Monitor task). An agent must
> not run `adb` without it; the `PreToolUse` guard denies it anyway.

**LAN over HTTPS (no cable).** `getUserMedia` and iOS's
`DeviceMotionEvent.requestPermission()` both require a secure context, and a
bare LAN IP over http is not one:

```bash
cd fw/sim && RGBX_SIM_SSL=1 npm run serve
# open https://<host-lan-ip>:5173 on the phone and accept the self-signed cert
```

`RGBX_SIM_SSL=1` is what enables `@vitejs/plugin-basic-ssl`; without it the
plugin is not registered at all.

## 5. What to check once it is on screen

- The picker lists the built extensions; selecting one activates it and the
  transport shows a non-zero fps with tick-wall stats climbing.
- **Brightness**: `Full (x1)` is the readable default. `Device (x0.02)` is the
  honest device view and is genuinely almost black — that is real hardware
  behaviour, not a bug. `Device, boosted` applies the same x0.02 *truncation*
  and then scales the result back up for the screen, which is how you see the
  hue-drift artefact from issue #259 without squinting.
- **Buttons**: arrow keys map to Up/Left/Right/Down, space to Wake; presses are
  edge-latched and delivered on the next tick.
- **Faults**: the hello extension's `Crash` / `Hang` bool params exercise the
  fault banner. A tick-time fault stops the loop, resets every param to its
  manifest default (the panel re-reads them), and only "Clear fault & retry"
  reloads the sandbox — the same rule as `ext select` on the device.
- **Audio**: the readout under the audio card shows live band energies and
  beat flags straight out of the firmware DSP. Metronome at 120 BPM should
  light band-0 beats about twice a second.

## Production build (the /sim/ Pages deployment)

`RGBX_SIM_BASE=/sim/ npx vite build` produces a fully functional static
bundle in `dist/browser/` — the mic worklet is imported with `?worker&url`
so it ships as its own transpiled chunk, and `publicDir` is `out/wasm` so
only the modules (flat, next to `index.html`) are copied in. To smoke it
locally under the same subpath the site uses:

```bash
fw/sim/build-extensions.sh
cd fw/sim && RGBX_SIM_BASE=/sim/ npx vite build
mkdir -p /tmp/composed && cp -r dist/browser /tmp/composed/sim
(cd /tmp/composed && python3 -m http.server 8000)   # -> http://localhost:8000/sim/
```

(Mic/DeviceMotion need a secure context; localhost qualifies.)

## Known limitations

- Live microphone input is deliberately non-deterministic — a mic run is for
  feel, not for reproducing a scenario. Use the Node CLI's canned scenarios
  when a run has to be repeatable.
- Uploaded `.wasm` extensions are session-only (by design): re-drop the file
  after a rebuild; nothing persists across page reloads.
