/**
 * Parameter panel, generated from the extension's own manifest metadata —
 * the browser counterpart of the app's generated GATT parameter UI and of
 * `ext param` on the shell.
 *
 * COLOR parameters are the only non-obvious case. The value written to the
 * host is a RAW mode-carrying word (see core/colorMode.ts):
 *
 *   bits 24-31  mode (0 Static, 1 SpectrumSweep, 2 RandomOnBeat,
 *                     3 RandomOnActivate, 4 RandomTimerFade)
 *   bits 16-23  speed, for every mode except Static
 *   bits  0-23  the 0x00RRGGBB colour, Static only
 *
 * For non-Static modes the colour is generated from a hue wheel and the
 * picker's value is unused, so the panel keeps colour/mode/speed as separate
 * UI state and recomposes the word on every edit — switching to a generated
 * mode and back doesn't destroy the colour you picked.
 */

import { RgbxParamType, getStringSlot } from "../../core/abi";
import { ColorMode } from "../../core/colorMode";
import type { SimHost } from "../../core/host";
import type { ParamInfo } from "../../core/manifest";

/** Slider range for UINT32 params. The ABI gives no bounds, so this is a
 * heuristic for the common "speed / count / length" style parameter; the
 * number box next to it accepts the full 32-bit range. */
const UINT_SLIDER_MAX = 500;

const COLOR_MODE_LABELS: [ColorMode, string][] = [
  [ColorMode.Static, "Static"],
  [ColorMode.SpectrumSweep, "Spectrum sweep"],
  [ColorMode.RandomOnBeat, "Random on beat"],
  [ColorMode.RandomOnActivate, "Random on activate"],
  [ColorMode.RandomTimerFade, "Random timer fade"],
];

export interface ColorState {
  mode: ColorMode;
  speed: number;
  rgb: number;
}

interface Row {
  info: ParamInfo;
  index: number;
  /** Re-reads the host's authoritative value into the controls. */
  sync(): void;
}

export function composeColorWord(mode: ColorMode, speed: number, rgb: number): number {
  if (mode === ColorMode.Static) {
    return (rgb & 0x00ffffff) >>> 0;
  }
  return (((mode & 0xff) << 24) | ((speed & 0xff) << 16) | (rgb & 0xffff)) >>> 0;
}

export class ParamPanel {
  private rows: Row[] = [];
  private host: SimHost | null = null;

  constructor(
    private readonly root: HTMLElement,
    /** Called after any edit, so the caller can refresh dependent UI. */
    private readonly onChange: () => void = () => {},
  ) {}

  /** Rebuilds the whole panel for a freshly activated extension. */
  build(host: SimHost): void {
    this.host = host;
    this.rows = [];
    this.root.replaceChildren();

    const params = host.metadata?.params ?? [];
    if (params.length === 0) {
      const empty = document.createElement("p");
      empty.className = "muted";
      empty.textContent = "This extension declares no parameters.";
      this.root.append(empty);
      return;
    }

    params.forEach((info, index) => {
      const row = document.createElement("div");
      row.className = "param";

      const label = document.createElement("div");
      label.className = "param-label";
      label.innerHTML = `<span class="param-name"></span><span class="param-type"></span>`;
      (label.querySelector(".param-name") as HTMLElement).textContent = info.name;
      (label.querySelector(".param-type") as HTMLElement).textContent = typeName(info.type);
      row.append(label);

      const body = document.createElement("div");
      body.className = "param-body";
      row.append(body);

      const chip = document.createElement("span");
      chip.className = "chip";
      row.append(chip);

      this.rows.push(this.buildControl(host, info, index, body, chip));
      this.root.append(row);
    });
    this.syncFromHost();
  }

  /** Re-reads every control from SimHost — needed after a tick-time fault,
   * which resets all params to their manifest defaults. */
  syncFromHost(): void {
    for (const row of this.rows) {
      row.sync();
    }
  }

  /** Writes the manifest defaults back through the normal setters, so the
   * panel and the host stay in step. */
  resetToDefaults(): void {
    const host = this.host;
    if (host === null || host.metadata === null) {
      return;
    }
    host.metadata.params.forEach((info, index) => {
      if (info.type === RgbxParamType.String) {
        const slot = info.stringSlot;
        host.setStringParam(index, host.metadata?.stringDefaults[slot] ?? "");
      } else {
        host.setParam(index, info.defaultValue);
      }
    });
    this.syncFromHost();
    this.onChange();
  }

  private buildControl(
    host: SimHost,
    info: ParamInfo,
    index: number,
    body: HTMLElement,
    chip: HTMLElement,
  ): Row {
    switch (info.type) {
      case RgbxParamType.Bool:
        return this.buildBool(host, info, index, body, chip);
      case RgbxParamType.String:
        return this.buildString(host, info, index, body, chip);
      case RgbxParamType.Color:
        return this.buildColor(host, info, index, body, chip);
      case RgbxParamType.Float:
        return this.buildFloat(host, info, index, body, chip);
      default:
        return this.buildUint(host, info, index, body, chip);
    }
  }

  /* buildUint cannot be reused for floats: its write path truncates
   * (Math.trunc + `>>> 0`), and paramValues[index] holds the float's raw bit
   * pattern, not a displayable number. */
  private buildFloat(
    host: SimHost,
    info: ParamInfo,
    index: number,
    body: HTMLElement,
    chip: HTMLElement,
  ): Row {
    const box = el<HTMLInputElement>("input", { type: "number", step: "any", class: "num" });
    body.append(box);

    box.addEventListener("change", () => {
      const v = Number(box.value);
      if (box.value !== "" && Number.isFinite(v)) {
        host.setParamF32(index, v);
      }
      sync();
      this.onChange();
    });

    const sync = () => {
      /* Round-trip through float32 already happened in the host slot; showing
       * up to 7 significant digits reproduces any float32 exactly without
       * trailing binary noise (same idea as the app's formatFloat32). */
      const v = Number(host.paramF32(index).toPrecision(7));
      box.value = String(v);
      chip.textContent = String(v);
    };
    return { info, index, sync };
  }

  private buildUint(
    host: SimHost,
    info: ParamInfo,
    index: number,
    body: HTMLElement,
    chip: HTMLElement,
  ): Row {
    const slider = el<HTMLInputElement>("input", { type: "range", min: "0", max: String(UINT_SLIDER_MAX), step: "1" });
    const box = el<HTMLInputElement>("input", { type: "number", min: "0", step: "1", class: "num" });
    body.append(slider, box);

    const write = (value: number) => {
      const v = Number.isFinite(value) ? Math.max(0, Math.trunc(value)) >>> 0 : 0;
      host.setParam(index, v);
      sync();
      this.onChange();
    };
    slider.addEventListener("input", () => write(Number(slider.value)));
    box.addEventListener("change", () => write(Number(box.value)));

    const sync = () => {
      const v = host.paramValues[index];
      box.value = String(v);
      slider.value = String(Math.min(v, UINT_SLIDER_MAX));
      chip.textContent = `${v}`;
    };
    return { info, index, sync };
  }

  private buildBool(
    host: SimHost,
    info: ParamInfo,
    index: number,
    body: HTMLElement,
    chip: HTMLElement,
  ): Row {
    const box = el<HTMLInputElement>("input", { type: "checkbox" });
    const wrap = el<HTMLLabelElement>("label", { class: "check" });
    wrap.append(box, document.createTextNode(" enabled"));
    body.append(wrap);

    box.addEventListener("change", () => {
      host.setParam(index, box.checked ? 1 : 0);
      sync();
      this.onChange();
    });

    const sync = () => {
      const on = host.paramValues[index] !== 0;
      box.checked = on;
      chip.textContent = on ? "true" : "false";
    };
    return { info, index, sync };
  }

  private buildString(
    host: SimHost,
    info: ParamInfo,
    index: number,
    body: HTMLElement,
    chip: HTMLElement,
  ): Row {
    // 31 characters + NUL is what RGBX_PARAM_STRING_MAX allows on the wire.
    const box = el<HTMLInputElement>("input", { type: "text", maxlength: "31", class: "text" });
    body.append(box);

    box.addEventListener("input", () => {
      host.setStringParam(index, box.value);
      sync();
      this.onChange();
    });

    const sync = () => {
      const slot = info.stringSlot;
      const value = getStringSlot(host.stringValues, slot);
      if (document.activeElement !== box) {
        box.value = value;
      }
      chip.textContent = `slot ${slot} · ${value.length}/31`;
    };
    return { info, index, sync };
  }

  private buildColor(
    host: SimHost,
    info: ParamInfo,
    index: number,
    body: HTMLElement,
    chip: HTMLElement,
  ): Row {
    const state: ColorState = decodeColorWord(info.defaultValue);

    const picker = el<HTMLInputElement>("input", { type: "color" });
    const mode = el<HTMLSelectElement>("select", {});
    for (const [value, text] of COLOR_MODE_LABELS) {
      const opt = document.createElement("option");
      opt.value = String(value);
      opt.textContent = text;
      mode.append(opt);
    }
    const speedWrap = el<HTMLLabelElement>("label", { class: "speed" });
    const speed = el<HTMLInputElement>("input", { type: "range", min: "0", max: "255", step: "1" });
    const speedValue = el<HTMLSpanElement>("span", { class: "speed-value" });
    speedWrap.append(document.createTextNode("speed"), speed, speedValue);
    body.append(picker, mode, speedWrap);

    const write = () => {
      host.setParam(index, composeColorWord(state.mode, state.speed, state.rgb));
      sync();
      this.onChange();
    };
    picker.addEventListener("input", () => {
      state.rgb = parseInt(picker.value.slice(1), 16) >>> 0;
      write();
    });
    mode.addEventListener("change", () => {
      state.mode = Number(mode.value) as ColorMode;
      write();
    });
    speed.addEventListener("input", () => {
      state.speed = Number(speed.value);
      write();
    });

    const sync = () => {
      // The host's raw word is authoritative (a fault resets it), so decode
      // it back — but keep the picker's colour for generated modes, which
      // don't carry one.
      const raw = host.paramValues[index];
      const decoded = decodeColorWord(raw);
      state.mode = decoded.mode;
      state.speed = decoded.speed;
      if (decoded.mode === ColorMode.Static) {
        state.rgb = decoded.rgb;
      }
      picker.value = `#${state.rgb.toString(16).padStart(6, "0")}`;
      mode.value = String(state.mode);
      speed.value = String(state.speed);
      speedValue.textContent = String(state.speed);
      speedWrap.classList.toggle("hidden", state.mode === ColorMode.Static);
      picker.classList.toggle("dimmed", state.mode !== ColorMode.Static);
      chip.textContent = `0x${raw.toString(16).padStart(8, "0")}`;
    };
    return { info, index, sync };
  }
}

/** Splits a raw COLOR word into UI state. Unknown mode bytes (including
 * 0xFF, the pre-feature persisted default) mean Static, matching
 * ColorModeResolver. */
export function decodeColorWord(raw: number): ColorState {
  const modeByte = (raw >>> 24) & 0xff;
  const known =
    modeByte >= ColorMode.SpectrumSweep && modeByte <= ColorMode.RandomTimerFade;
  if (!known) {
    return { mode: ColorMode.Static, speed: 128, rgb: raw & 0x00ffffff };
  }
  return { mode: modeByte as ColorMode, speed: (raw >>> 16) & 0xff, rgb: 0xffffff };
}

function typeName(type: RgbxParamType): string {
  switch (type) {
    case RgbxParamType.Uint32: return "uint32";
    case RgbxParamType.Color: return "color";
    case RgbxParamType.Bool: return "bool";
    case RgbxParamType.String: return "string";
    case RgbxParamType.Float: return "float32";
    default: return `type ${type}`;
  }
}

function el<T extends HTMLElement>(tag: string, attrs: Record<string, string>): T {
  const node = document.createElement(tag) as T;
  for (const [k, v] of Object.entries(attrs)) {
    node.setAttribute(k, v);
  }
  return node;
}
