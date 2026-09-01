/**
 * Timeline execution for scenario runs — param writes by name and button
 * presses fired when simTime reaches each event's atMs. Platform-agnostic,
 * shared by the Node CLI and the browser scenario player.
 */

import { RgbxParamType } from "./abi";
import { SimHost } from "./host";
import { BUTTON_INDEX, ScenarioEvent } from "./scenario";

/** Parses a scalar param value: decimal, 0x hex, or true/false. Returns
 * null when the token is not scalar-shaped. NEVER applied to STRING params
 * — their values pass through verbatim (a STRING param legitimately holds
 * the literal text "true" or "0x10"). */
export function parseScalarParamValue(raw: number | string): number | null {
  if (typeof raw === "number") {
    return raw;
  }
  if (/^(0x[0-9a-fA-F]+|\d+)$/.test(raw)) {
    return Number(raw);
  }
  if (raw === "true") {
    return 1;
  }
  if (raw === "false") {
    return 0;
  }
  return null;
}

export function applyParam(host: SimHost, name: string, rawValue: number | string): string | null {
  const idx = host.paramIndexByName(name);
  if (idx < 0) {
    return `no param named "${name}" (have: ${host.metadata?.params.map((p) => p.name).join(", ")})`;
  }
  // Type first, coercion second: STRING params take the raw token verbatim.
  const type = host.metadata!.params[idx].type;
  if (type === RgbxParamType.String) {
    host.setStringParam(idx, String(rawValue));
    return null;
  }
  if (type === RgbxParamType.Float) {
    // Floats get their own parse: "0.5" is not scalar-shaped to
    // parseScalarParamValue (nor should it become so — 0.5 on a UINT32
    // param must stay an error, not a silent truncation), and setParam's
    // `>>> 0` would zero it. Non-finite values are rejected like the
    // device's GATT write path.
    const value = typeof rawValue === "number" ? rawValue : Number(rawValue);
    if (rawValue === "" || !Number.isFinite(value)) {
      return `param "${name}" expects a finite float, got "${rawValue}"`;
    }
    host.setParamF32(idx, value);
    return null;
  }
  const value = parseScalarParamValue(rawValue);
  if (value === null) {
    return `param "${name}" expects a number (decimal, 0x hex, or true/false), got "${rawValue}"`;
  }
  host.setParam(idx, value);
  return null;
}

/** Timeline events due at or before the CURRENT sim time fire before the tick
 * that first covers them, each exactly once. One instance per run — it holds
 * the fired-through cursor, so a restart needs a fresh TimelineRunner. */
export class TimelineRunner {
  private readonly events: ScenarioEvent[];
  private at = 0;

  constructor(events: ScenarioEvent[] | undefined) {
    this.events = [...(events ?? [])].sort((a, b) => a.atMs - b.atMs);
  }

  /** Fires every due event; throws on a bad param name/value. Returns which
   * kinds fired so a UI caller knows to re-sync its params panel. */
  pump(host: SimHost): { firedSet: boolean; firedPress: boolean } {
    let firedSet = false;
    let firedPress = false;
    while (this.at < this.events.length && this.events[this.at].atMs <= host.simTimeMs) {
      const ev = this.events[this.at++];
      if (ev.set !== undefined) {
        for (const [name, value] of Object.entries(ev.set)) {
          const err = applyParam(host, name, value);
          if (err !== null) {
            throw new Error(`timeline @${ev.atMs}ms: ${err}`);
          }
        }
        firedSet = true;
      }
      if (ev.press !== undefined) {
        host.pressButton(BUTTON_INDEX[ev.press]);
        firedPress = true;
      }
    }
    return { firedSet, firedPress };
  }
}
