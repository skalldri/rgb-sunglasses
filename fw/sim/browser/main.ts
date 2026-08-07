/**
 * Browser simulator entry point: wires the DOM to a SimHost running the
 * selected extension in a Web Worker sandbox.
 *
 * The run loop is a self-paced async loop rather than requestAnimationFrame:
 * the sim's clock is nominal (exactly dtMs per tick, like the device's
 * render tick), so pacing to real time is presentation only. Falling behind
 * real time therefore slows the wall-clock animation but never changes what
 * the extension computes — the same property the Node CLI relies on for
 * reproducible runs.
 */

import { BUTTON_NAMES } from "../core/abi";
import {
  DEFAULT_BRIGHTNESS_FACTOR,
  FRAME_BYTES,
  toDisplayedFrame,
} from "../core/display";
import { SimHost } from "../core/host";
import type { FaultInfo, TickOutcome } from "../core/host";
import { GlassesRenderer } from "./render/glasses";
import {
  AudioSourceKind,
  AudioSources,
  TappedAudioProvider,
  frameIndexForSimTime,
} from "./sensors/audio";
import { G, ImuManager, ImuSourceKind, requestMotionPermission } from "./sensors/imu";
import { ConsolePanel } from "./ui/console";
import { ParamPanel } from "./ui/params";
import { BrowserWorkerAdapter } from "./workerAdapter";

/** kTargetRenderIntervalMs truncated — the device's nominal render period. */
const DT_MS = 11;
/** Repaint every Nth tick when display decimation is on (~30 Hz of 90 Hz). */
const DECIMATION = 3;
/** Device brightness is a x0.02 truncating scale; the boosted view undoes
 * the scale (but not the truncation) so the quantization stays visible. */
const BOOST = Math.round(1 / DEFAULT_BRIGHTNESS_FACTOR);

interface WasmEntry {
  name: string;
  url: string;
  size: number;
}

function must<T extends HTMLElement>(id: string): T {
  const node = document.getElementById(id);
  if (node === null) {
    throw new Error(`missing element #${id}`);
  }
  return node as T;
}

const els = {
  extSelect: must<HTMLSelectElement>("ext-select"),
  extReload: must<HTMLButtonElement>("ext-reload"),
  extMeta: must<HTMLElement>("ext-meta"),
  fault: must<HTMLElement>("fault"),
  faultKind: must<HTMLElement>("fault-kind"),
  faultDetail: must<HTMLElement>("fault-detail"),
  faultClear: must<HTMLButtonElement>("fault-clear"),
  canvas: must<HTMLCanvasElement>("glasses"),
  play: must<HTMLButtonElement>("play"),
  step: must<HTMLButtonElement>("step"),
  brightness: must<HTMLSelectElement>("brightness"),
  decimate: must<HTMLInputElement>("decimate"),
  stats: must<HTMLElement>("stats"),
  buttons: must<HTMLElement>("buttons"),
  params: must<HTMLElement>("params"),
  paramReset: must<HTMLButtonElement>("param-reset"),
  console: must<HTMLElement>("console"),
  consoleClear: must<HTMLButtonElement>("console-clear"),
  audioSource: must<HTMLSelectElement>("audio-source"),
  audioBpm: must<HTMLInputElement>("audio-bpm"),
  audioBpmValue: must<HTMLElement>("audio-bpm-value"),
  audioGain: must<HTMLInputElement>("audio-gain"),
  audioGainValue: must<HTMLElement>("audio-gain-value"),
  audioFile: must<HTMLInputElement>("audio-file"),
  audioFileInfo: must<HTMLElement>("audio-file-info"),
  micToggle: must<HTMLButtonElement>("mic-toggle"),
  micLevel: must<HTMLElement>("mic-level"),
  audioReadout: must<HTMLElement>("audio-readout"),
  imuSource: must<HTMLSelectElement>("imu-source"),
  imuStatic: must<HTMLElement>("imu-static"),
  imuPermission: must<HTMLButtonElement>("imu-permission"),
  imuReadout: must<HTMLElement>("imu-readout"),
};

const renderer = new GlassesRenderer(els.canvas);
const consolePanel = new ConsolePanel(els.console);
const paramPanel = new ParamPanel(els.params);
const audio = new AudioSources();
const audioTap = new TappedAudioProvider(() => audio.provider);
const imu = new ImuManager();

let host: SimHost | null = null;
let running = false;
let ticking = false;
let loopToken = 0;
let nextDueMs = 0;
let lastRaw: Uint8Array = new Uint8Array(FRAME_BYTES);
let goodMoment = false;
let manifestIntact = true;

const wall = { min: Number.POSITIVE_INFINITY, max: 0, sum: 0, count: 0 };
let ticksSinceSample = 0;
let lastFpsSampleMs = performance.now();
let fps = 0;

/* ------------------------------------------------------------------ */
/* Extension loading                                                    */
/* ------------------------------------------------------------------ */

async function loadExtensionList(): Promise<WasmEntry[]> {
  try {
    const resp = await fetch("/wasm-index.json");
    const all = (await resp.json()) as WasmEntry[];
    // audio_dsp.wasm is the DSP the audio sources run on, not an extension.
    return all.filter((e) => !e.name.startsWith("audio_dsp"));
  } catch {
    return [];
  }
}

async function selectExtension(entry: WasmEntry): Promise<void> {
  stop();
  loopToken++;
  if (host !== null) {
    await host.terminate();
    host = null;
  }
  consolePanel.note(`loading ${entry.name} (${entry.size} bytes)`);

  let bytes: ArrayBuffer;
  try {
    const resp = await fetch(entry.url);
    if (!resp.ok) {
      throw new Error(`HTTP ${resp.status}`);
    }
    bytes = await resp.arrayBuffer();
  } catch (err) {
    showFault({
      kind: "load_failed",
      detail: `could not fetch ${entry.url}: ${String(err)}`,
      tick: -1,
      paramsResetToDefaults: false,
    });
    return;
  }

  const h = new SimHost({
    wasmBytes: bytes,
    adapterFactory: () => new BrowserWorkerAdapter(),
    dtMs: DT_MS,
    audioProvider: audioTap,
    imuProvider: imu,
  });
  const fault = await h.activate();
  host = h;
  resetStats();
  lastRaw = new Uint8Array(FRAME_BYTES);
  paramPanel.build(h);
  updateMeta();

  if (fault !== null) {
    showFault(fault);
    paint();
    return;
  }
  hideFault();
  consolePanel.note(
    `activated "${h.metadata?.displayName}" — ${h.metadata?.width}x${h.metadata?.height}, ` +
      `${h.metadata?.paramCount} params`,
  );
  start();
}

function updateMeta(): void {
  const meta = host?.metadata;
  if (meta === undefined || meta === null) {
    els.extMeta.textContent = "not loaded";
    return;
  }
  els.extMeta.textContent = `${meta.displayName} · ${meta.width}x${meta.height} · ${meta.paramCount} params · ${meta.stringParamCount} strings`;
}

/* ------------------------------------------------------------------ */
/* Run loop                                                             */
/* ------------------------------------------------------------------ */

function start(): void {
  if (running || host === null) {
    return;
  }
  running = true;
  nextDueMs = performance.now();
  els.play.textContent = "Pause";
  void pump(++loopToken);
}

function stop(): void {
  running = false;
  els.play.textContent = "Play";
}

async function pump(token: number): Promise<void> {
  while (running && token === loopToken) {
    await stepOnce();
    if (!running || token !== loopToken) {
      break;
    }
    nextDueMs += DT_MS;
    const now = performance.now();
    // A long stall (background tab, a slow tick) must not turn into a burst
    // of catch-up ticks — resync the schedule instead.
    if (nextDueMs < now - 200) {
      nextDueMs = now;
    }
    const delay = nextDueMs - now;
    if (delay > 0) {
      await sleep(delay);
    }
  }
}

async function stepOnce(): Promise<void> {
  const h = host;
  // SimHost is not reentrant — the Step button must not start a second tick
  // while the pump loop is still awaiting one.
  if (h === null || ticking) {
    return;
  }
  ticking = true;
  let outcome: TickOutcome;
  try {
    outcome = await h.tick();
  } catch (err) {
    // Switching extensions terminates the adapter out from under an
    // in-flight request; that rejection is expected and not a fault.
    if (h === host) {
      consolePanel.note(`tick failed: ${String(err)}`);
      stop();
    }
    return;
  } finally {
    ticking = false;
  }
  // The extension may have been swapped while this tick was in flight.
  if (h !== host) {
    return;
  }
  consolePanel.append(outcome.log);

  if (outcome.status === "fault") {
    stop();
    // Every tick-time fault resets params to manifest defaults, so the
    // panel has to re-read them or it would show stale values.
    paramPanel.syncFromHost();
    showFault(outcome.fault);
    return;
  }

  lastRaw = outcome.framebuffer;
  goodMoment = outcome.goodMoment;
  manifestIntact = outcome.manifestIntact;
  wall.min = Math.min(wall.min, outcome.wallMs);
  wall.max = Math.max(wall.max, outcome.wallMs);
  wall.sum += outcome.wallMs;
  wall.count++;
  ticksSinceSample++;

  if (!els.decimate.checked || h.tickIndex % DECIMATION === 0) {
    paint();
  }
}

function paint(): void {
  renderer.draw(displayedFrame(lastRaw));
}

function displayedFrame(raw: Uint8Array): Uint8Array {
  const mode = els.brightness.value;
  if (mode === "full") {
    return toDisplayedFrame(raw, 1);
  }
  const dim = toDisplayedFrame(raw, DEFAULT_BRIGHTNESS_FACTOR);
  if (mode === "device") {
    return dim;
  }
  const out = new Uint8Array(dim.length);
  for (let i = 0; i < dim.length; i++) {
    out[i] = Math.min(255, dim[i] * BOOST);
  }
  return out;
}

function resetStats(): void {
  wall.min = Number.POSITIVE_INFINITY;
  wall.max = 0;
  wall.sum = 0;
  wall.count = 0;
  ticksSinceSample = 0;
  lastFpsSampleMs = performance.now();
  fps = 0;
}

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

/* ------------------------------------------------------------------ */
/* Faults                                                               */
/* ------------------------------------------------------------------ */

function showFault(fault: FaultInfo): void {
  els.fault.classList.remove("hidden");
  els.faultKind.textContent = fault.kind;
  const where = fault.tick >= 0 ? ` (tick ${fault.tick})` : "";
  const reset = fault.paramsResetToDefaults ? " — params reset to defaults" : "";
  els.faultDetail.textContent = `${fault.detail}${where}${reset}`;
  consolePanel.note(`FAULT ${fault.kind}: ${fault.detail}`);
  updateStats();
}

function hideFault(): void {
  els.fault.classList.add("hidden");
}

async function clearFaultAndRetry(): Promise<void> {
  const h = host;
  if (h === null) {
    return;
  }
  h.clearFault();
  const fault = await h.activate();
  paramPanel.build(h);
  updateMeta();
  if (fault !== null) {
    showFault(fault);
    return;
  }
  hideFault();
  consolePanel.note("fault cleared, extension re-activated");
  resetStats();
  start();
}

/* ------------------------------------------------------------------ */
/* Periodic UI refresh (stats, readouts, console flush)                 */
/* ------------------------------------------------------------------ */

function updateStats(): void {
  const now = performance.now();
  const elapsed = now - lastFpsSampleMs;
  if (elapsed >= 250) {
    fps = (ticksSinceSample * 1000) / elapsed;
    ticksSinceSample = 0;
    lastFpsSampleMs = now;
  }

  const avg = wall.count === 0 ? 0 : wall.sum / wall.count;
  const min = wall.count === 0 ? 0 : wall.min;
  const rows: [string, string, string?][] = [
    ["tick", host === null ? "—" : `${host.tickIndex}`],
    ["sim time", host === null ? "—" : `${(host.simTimeMs / 1000).toFixed(2)} s`],
    ["fps", fps.toFixed(1)],
    ["tick wall min/avg/max", `${min.toFixed(2)} / ${avg.toFixed(2)} / ${wall.max.toFixed(2)} ms`],
    ["good_moment", goodMoment ? "yes" : "no", goodMoment ? "ok" : "off"],
    ["manifest", manifestIntact ? "intact" : "MODIFIED", manifestIntact ? "ok" : "bad"],
  ];
  els.stats.replaceChildren(
    ...rows.flatMap(([label, value, tone]) => {
      const k = document.createElement("span");
      k.className = "stat-key";
      k.textContent = label;
      const v = document.createElement("span");
      v.className = `stat-value${tone === undefined ? "" : ` tone-${tone}`}`;
      v.textContent = value;
      return [k, v];
    }),
  );
}

function updateReadouts(): void {
  const f = audioTap.last;
  const bands = Array.from(f.bandEnergy, (v) => v.toExponential(1)).join("  ");
  const beats = Array.from(f.beat, (v) => (v !== 0 ? "1" : "·")).join(" ");
  els.audioReadout.textContent = `band energy  ${bands}\nbeat         ${beats}${
    audio.dspReady ? "" : "\n(audio_dsp.wasm not loaded — silence only)"
  }`;
  els.micLevel.style.width = `${Math.min(100, audio.micLevel * 300).toFixed(1)}%`;

  const s = imu.last;
  const fmt = (v: number) => v.toFixed(2).padStart(7);
  els.imuReadout.textContent =
    `accel  ${fmt(s.accel[0])} ${fmt(s.accel[1])} ${fmt(s.accel[2])}  m/s²\n` +
    `gyro   ${fmt(s.gyro[0])} ${fmt(s.gyro[1])} ${fmt(s.gyro[2])}  rad/s`;
}

/* ------------------------------------------------------------------ */
/* Input panels                                                         */
/* ------------------------------------------------------------------ */

function syncDataFor(scope: HTMLElement, kind: string): void {
  scope.querySelectorAll<HTMLElement>("[data-for]").forEach((node) => {
    const list = (node.dataset.for ?? "").split(/\s+/);
    node.classList.toggle("hidden", !list.includes(kind));
  });
}

function buildButtons(): void {
  const keys = ["↑", "←", "→", "↓", "space"];
  BUTTON_NAMES.forEach((name, i) => {
    const btn = document.createElement("button");
    btn.className = "btn btn-pad";
    btn.innerHTML = `<span>${name}</span><small>${keys[i]}</small>`;
    btn.addEventListener("click", () => host?.pressButton(i));
    els.buttons.append(btn);
  });
}

function buildImuSliders(): void {
  const axes: [string, "accel" | "gyro", number, number, number][] = [
    ["accel x", "accel", 0, 20, 0],
    ["accel y", "accel", 1, 20, 0],
    ["accel z", "accel", 2, 20, G],
    ["gyro x", "gyro", 0, 10, 0],
    ["gyro y", "gyro", 1, 10, 0],
    ["gyro z", "gyro", 2, 10, 0],
  ];
  for (const [label, field, index, range, initial] of axes) {
    const wrap = document.createElement("label");
    wrap.className = "field";
    const span = document.createElement("span");
    const value = document.createElement("b");
    value.textContent = initial.toFixed(2);
    span.append(document.createTextNode(`${label} `), value);
    const slider = document.createElement("input");
    slider.type = "range";
    slider.min = String(-range);
    slider.max = String(range);
    slider.step = "0.01";
    slider.value = String(initial);
    slider.addEventListener("input", () => {
      const v = Number(slider.value);
      imu.staticSource[field][index] = v;
      value.textContent = v.toFixed(2);
    });
    wrap.append(span, slider);
    els.imuStatic.append(wrap);
  }
}

function wireAudioPanel(): void {
  const card = els.audioSource.closest(".card") as HTMLElement;
  const applyKind = (kind: AudioSourceKind) => {
    syncDataFor(card, kind);
    if (kind !== "mic") {
      audio.setSource({ kind });
    }
  };

  els.audioSource.addEventListener("change", () => {
    const kind = els.audioSource.value as AudioSourceKind;
    if (kind === "mic") {
      syncDataFor(card, kind);
      if (!audio.micActive) {
        consolePanel.note("microphone selected — press \"Enable microphone\" to grant access");
      } else {
        audio.setSource({ kind });
      }
      return;
    }
    applyKind(kind);
    consolePanel.note(`audio source: ${kind}`);
  });

  els.audioBpm.addEventListener("input", () => {
    const bpm = Number(els.audioBpm.value);
    els.audioBpmValue.textContent = String(bpm);
    audio.setSource({ bpm }, true);
  });
  els.audioGain.addEventListener("input", () => {
    const gainDb = Number(els.audioGain.value);
    els.audioGainValue.textContent = String(gainDb);
    audio.setSource({ gainDb }, true);
  });

  els.audioFile.addEventListener("change", () => {
    const file = els.audioFile.files?.[0];
    if (file === undefined) {
      return;
    }
    void audio
      .loadFile(file)
      .then((seconds) => {
        els.audioFileInfo.textContent = `${file.name} — ${seconds.toFixed(1)} s @ 16 kHz mono`;
        els.audioSource.value = "file";
        syncDataFor(card, "file");
        consolePanel.note(`audio file loaded: ${file.name}`);
      })
      .catch((err: unknown) => {
        els.audioFileInfo.textContent = `decode failed: ${String(err)}`;
      });
  });

  els.micToggle.addEventListener("click", () => {
    if (audio.micActive) {
      audio.stopMic();
      els.micToggle.textContent = "Enable microphone";
      els.audioSource.value = audio.settings.kind;
      syncDataFor(card, audio.settings.kind);
      return;
    }
    void audio
      .startMic()
      .then(() => {
        els.micToggle.textContent = "Disable microphone";
        els.audioSource.value = "mic";
        syncDataFor(card, "mic");
        consolePanel.note("microphone capture started");
      })
      .catch((err: unknown) => {
        consolePanel.note(`microphone unavailable: ${String(err)}`);
      });
  });

  syncDataFor(card, audio.settings.kind);
}

function wireImuPanel(): void {
  const card = els.imuSource.closest(".card") as HTMLElement;
  els.imuSource.addEventListener("change", () => {
    const kind = els.imuSource.value as ImuSourceKind;
    imu.setKind(kind);
    syncDataFor(card, kind);
    consolePanel.note(`IMU source: ${kind}`);
  });
  els.imuPermission.addEventListener("click", () => {
    void requestMotionPermission().then((granted) => {
      consolePanel.note(granted ? "motion access granted" : "motion access denied");
      if (granted) {
        // Re-arm: the listeners attach only after permission on iOS.
        const kind = els.imuSource.value as ImuSourceKind;
        imu.setKind("static");
        imu.setKind(kind);
      }
    });
  });

  // Tilt-from-mouse: drag anywhere on the panel.
  let dragging = false;
  const point = (ev: PointerEvent) => {
    const { nx, ny } = renderer.normalizedPoint(ev.clientX, ev.clientY);
    imu.mouse.setPoint(nx, ny);
  };
  els.canvas.addEventListener("pointerdown", (ev) => {
    if (imu.sourceKind !== "mouse") {
      return;
    }
    dragging = true;
    els.canvas.setPointerCapture(ev.pointerId);
    point(ev);
  });
  els.canvas.addEventListener("pointermove", (ev) => {
    if (dragging) {
      point(ev);
    }
  });
  els.canvas.addEventListener("pointerup", (ev) => {
    dragging = false;
    els.canvas.releasePointerCapture(ev.pointerId);
  });

  syncDataFor(card, imu.sourceKind);
}

function wireTabs(): void {
  document.querySelectorAll<HTMLButtonElement>(".tab").forEach((tab) => {
    tab.addEventListener("click", () => {
      document.querySelectorAll(".tab").forEach((t) => t.classList.remove("active"));
      document.querySelectorAll(".tab-panel").forEach((p) => p.classList.remove("active"));
      tab.classList.add("active");
      must(`tab-${tab.dataset.tab}`).classList.add("active");
    });
  });
}

function wireKeyboard(): void {
  const map: Record<string, number | undefined> = {
    ArrowUp: 0,
    ArrowLeft: 1,
    ArrowRight: 2,
    ArrowDown: 3,
    " ": 4,
  };
  window.addEventListener("keydown", (ev) => {
    const target = ev.target as HTMLElement | null;
    // Never steal keys from a text field the user is editing.
    if (target !== null && /^(INPUT|TEXTAREA|SELECT)$/.test(target.tagName)) {
      return;
    }
    const id = map[ev.key];
    if (id === undefined || ev.repeat) {
      return;
    }
    ev.preventDefault();
    host?.pressButton(id);
    els.buttons.children[id]?.classList.add("pressed");
    setTimeout(() => els.buttons.children[id]?.classList.remove("pressed"), 120);
  });
}

/* ------------------------------------------------------------------ */
/* Boot                                                                 */
/* ------------------------------------------------------------------ */

async function boot(): Promise<void> {
  buildButtons();
  buildImuSliders();
  wireTabs();
  wireKeyboard();
  wireAudioPanel();
  wireImuPanel();

  audio.setFrameClock(() => (host === null ? 0 : frameIndexForSimTime(host.simTimeMs)));
  if (!(await audio.loadDsp())) {
    consolePanel.note(
      "audio_dsp.wasm not available — audio sources are disabled. Run fw/sim/build-extensions.sh.",
    );
  }

  els.play.addEventListener("click", () => (running ? stop() : start()));
  els.step.addEventListener("click", () => {
    stop();
    void stepOnce().then(paint);
  });
  els.brightness.addEventListener("change", paint);
  els.decimate.addEventListener("change", paint);
  els.paramReset.addEventListener("click", () => paramPanel.resetToDefaults());
  els.consoleClear.addEventListener("click", () => consolePanel.clear());
  els.faultClear.addEventListener("click", () => void clearFaultAndRetry());

  new ResizeObserver(() => {
    renderer.resize();
    paint();
  }).observe(els.canvas);

  const entries = await loadExtensionList();
  const selected = () => entries.find((e) => e.url === els.extSelect.value);
  els.extReload.addEventListener("click", () => {
    const entry = selected();
    if (entry !== undefined) {
      void selectExtension(entry);
    }
  });

  if (entries.length === 0) {
    els.extMeta.textContent = "no .wasm modules in fw/sim/out/wasm — run fw/sim/build-extensions.sh";
    consolePanel.note("extension index is empty; nothing to run");
  } else {
    for (const entry of entries) {
      const opt = document.createElement("option");
      opt.value = entry.url;
      opt.textContent = entry.name.replace(/\.wasm$/, "");
      els.extSelect.append(opt);
    }
    els.extSelect.addEventListener("change", () => {
      const entry = selected();
      if (entry !== undefined) {
        void selectExtension(entry);
      }
    });
    await selectExtension(entries[0]);
  }

  // One timer drives every non-frame-critical UI update, so a chatty
  // extension can't make the DOM the bottleneck.
  setInterval(() => {
    updateStats();
    updateReadouts();
    consolePanel.flush();
  }, 200);
}

void boot();
