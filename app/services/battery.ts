// Battery telemetry derivations for the Battery BLE service (issue #97).
//
// The firmware exposes only raw integer measurements (mV / mA / raw BQ25792
// CHG_STAT) — percentage, watts, and human-readable labels are all derived
// here, app-side.

// 2S LiPo resting-voltage discharge curve, [mV, %], highest first. Standard
// single-cell LiPo open-circuit-voltage curve times two (4.2 V/cell full,
// ~3.27 V/cell empty). Resting voltage: under load the pack sags, so the
// displayed percentage reads slightly low while the glasses are running —
// acceptable for a status display.
const DISCHARGE_CURVE_2S: readonly (readonly [number, number])[] = [
  [8400, 100],
  [8300, 95],
  [8220, 90],
  [8160, 85],
  [8050, 80],
  [7970, 75],
  [7910, 70],
  [7830, 65],
  [7750, 60],
  [7710, 55],
  [7670, 50],
  [7630, 45],
  [7590, 40],
  [7570, 35],
  [7530, 30],
  [7490, 25],
  [7450, 20],
  [7410, 15],
  [7370, 10],
  [7220, 5],
  [6550, 0],
];

/** Converts a 2S LiPo pack voltage (mV) to an estimated charge percentage [0..100]. */
export function voltageToPercent(mv: number): number {
  if (mv >= DISCHARGE_CURVE_2S[0][0]) return 100;
  const last = DISCHARGE_CURVE_2S[DISCHARGE_CURVE_2S.length - 1];
  if (mv <= last[0]) return 0;

  for (let i = 0; i < DISCHARGE_CURVE_2S.length - 1; i++) {
    const [hiMv, hiPct] = DISCHARGE_CURVE_2S[i];
    const [loMv, loPct] = DISCHARGE_CURVE_2S[i + 1];
    if (mv <= hiMv && mv >= loMv) {
      // Linear interpolation between the two surrounding curve points
      const t = (mv - loMv) / (hiMv - loMv);
      return Math.round(loPct + t * (hiPct - loPct));
    }
  }
  return 0; // unreachable given the guards above
}

/** Power in watts from millivolts × milliamps. Sign follows the current's sign. */
export function batteryWatts(mv: number, ma: number): number {
  return (mv / 1000) * (ma / 1000);
}

/**
 * Total system power consumption in watts.
 *
 * On battery (no USB input): IBAT is negative (discharging), IBUS ~0, so this
 * reduces to the battery's discharge power. On USB: the system runs from VBUS
 * while the battery charges, so consumption is USB input power minus the power
 * being banked into the battery. Converter losses are counted as consumption.
 */
export function systemWatts(vbusMv: number, ibusMa: number, vbatMv: number, ibatMa: number): number {
  return batteryWatts(vbusMv, ibusMa) - batteryWatts(vbatMv, ibatMa);
}

// Raw BQ25792 CHG_STAT field values (CHARGER_STATUS_1, 0-7). Must match the
// Bq25792ChargeStatus enum in fw/src/power.cpp.
export const CHARGE_STATUS_LABELS: Record<number, string> = {
  0: 'Not Charging',
  1: 'Trickle Charge',
  2: 'Pre-charge',
  3: 'Fast Charge (CC)',
  4: 'Taper Charge (CV)',
  5: 'Reserved',
  6: 'Top-off',
  7: 'Charge Done',
};

export function chargeStatusLabel(chgStat: number): string {
  return CHARGE_STATUS_LABELS[chgStat] ?? `Unknown (${chgStat})`;
}

// "Power Flags" bitmask from the firmware's Power Debug service (must match
// fw/src/bluetooth/power_debug_service.h).
export const POWER_FLAG_VBAT_PRESENT = 0x01;
export const POWER_FLAG_VBUS_PRESENT = 0x02;
export const POWER_FLAG_CHARGE_GATED = 0x40;

/**
 * Whether a battery pack is physically connected.
 *
 * Prefers the firmware's authoritative VBAT_PRESENT flag (Power Debug
 * service); older firmware without that service falls back to the same
 * heuristic the firmware's status LED uses (a healthy 2S pack never reads
 * below 6 V — see fw/src/power.cpp). Unknown inputs report true so the UI
 * doesn't cry "No Battery" on missing data.
 */
export function batteryPresent(powerFlags: number | null, vbatMv: number | null): boolean {
  if (powerFlags != null) return (powerFlags & POWER_FLAG_VBAT_PRESENT) !== 0;
  if (vbatMv != null) return vbatMv >= 6000;
  return true;
}

/**
 * Human-readable decode of the Power Flags bitmask, in bit order. Bits mirror
 * fw/src/bluetooth/power_debug_service.h.
 */
export function powerFlagLabels(flags: number): string[] {
  const names: readonly (readonly [number, string])[] = [
    [0x01, 'Battery Present'],
    [0x02, 'USB Power Present'],
    [0x04, 'Input Current Limited'],
    [0x08, 'Input Voltage Limited'],
    [0x10, 'Min-System-Voltage Regulation'],
    [0x20, 'Watchdog Expired'],
    [0x40, 'Charging Gated (No Battery)'],
  ];
  const labels = names.filter(([bit]) => (flags & bit) !== 0).map(([, name]) => name);
  return labels.length > 0 ? labels : ['None'];
}

// Mirrors enum tps25750_power_source (fw tps25750.h) as surfaced by the
// Power Debug service's "PD Source Type" characteristic.
const PD_SOURCE_LABELS: Record<number, string> = {
  0: 'Disconnected',
  1: 'USB Default (500 mA)',
  2: 'Type-C 1.5 A',
  3: 'Type-C 3.0 A',
  4: 'PD Contract',
  5: 'Unknown Contract',
};

export function pdSourceLabel(source: number): string {
  return PD_SOURCE_LABELS[source] ?? `Unknown (${source})`;
}

export type ChargeDirection = 'charging' | 'discharging' | 'idle' | 'done';

/**
 * Overall charge/discharge indication, combining the charger state machine
 * ("why") with the battery current's sign ("which way power is flowing").
 */
export function chargeDirection(ibatMa: number, chgStat: number): ChargeDirection {
  if (chgStat === 7) return 'done';
  if ((chgStat >= 1 && chgStat <= 4) || chgStat === 6) return 'charging';
  if (ibatMa < 0) return 'discharging';
  if (ibatMa > 0) return 'charging';
  return 'idle';
}

/** "7.91 V" from 7914 mV. */
export function formatVolts(mv: number): string {
  return `${(mv / 1000).toFixed(2)} V`;
}

/** "2.77 W" from 2.769... . */
export function formatWatts(watts: number): string {
  return `${watts.toFixed(2)} W`;
}
