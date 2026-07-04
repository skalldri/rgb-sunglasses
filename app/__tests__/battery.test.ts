import {
  batteryWatts,
  CHARGE_STATUS_LABELS,
  chargeDirection,
  chargeStatusLabel,
  formatVolts,
  formatWatts,
  systemWatts,
  voltageToPercent,
} from '@/services/battery';

describe('voltageToPercent', () => {
  it('maps the curve endpoints', () => {
    expect(voltageToPercent(8400)).toBe(100);
    expect(voltageToPercent(6550)).toBe(0);
  });

  it('clamps out-of-range voltages', () => {
    expect(voltageToPercent(9000)).toBe(100);
    expect(voltageToPercent(6000)).toBe(0);
    expect(voltageToPercent(0)).toBe(0);
  });

  it('returns exact table values at curve points', () => {
    expect(voltageToPercent(7670)).toBe(50);
    expect(voltageToPercent(7910)).toBe(70);
    expect(voltageToPercent(7370)).toBe(10);
  });

  it('interpolates linearly between curve points', () => {
    // Midpoint of [8300, 95] and [8400, 100]
    expect(voltageToPercent(8350)).toBe(98); // 97.5 rounds to 98
    // Midpoint of [7630, 45] and [7670, 50]
    expect(voltageToPercent(7650)).toBe(48); // 47.5 rounds to 48
  });

  it('is monotonically non-decreasing across the full range', () => {
    let prev = voltageToPercent(6000);
    for (let mv = 6000; mv <= 8500; mv += 10) {
      const pct = voltageToPercent(mv);
      expect(pct).toBeGreaterThanOrEqual(prev);
      prev = pct;
    }
  });
});

describe('batteryWatts / systemWatts', () => {
  it('computes watts from mV × mA with the current sign preserved', () => {
    expect(batteryWatts(7910, -350)).toBeCloseTo(-2.7685);
    expect(batteryWatts(7910, 350)).toBeCloseTo(2.7685);
    expect(batteryWatts(0, 500)).toBe(0);
  });

  it('on battery, system power equals the battery discharge power', () => {
    // No USB input, 350 mA discharge at 7.91 V
    expect(systemWatts(0, 0, 7910, -350)).toBeCloseTo(2.7685);
  });

  it('on USB, system power is input power minus power banked into the battery', () => {
    // 5 V × 1 A in, 500 mA charging at 7.91 V
    expect(systemWatts(5000, 1000, 7910, 500)).toBeCloseTo(5.0 - 3.955);
  });
});

describe('charge status labels', () => {
  it('covers all 8 raw CHG_STAT values', () => {
    for (let stat = 0; stat <= 7; stat++) {
      expect(CHARGE_STATUS_LABELS[stat]).toBeTruthy();
    }
  });

  it('falls back gracefully for unknown values', () => {
    expect(chargeStatusLabel(42)).toBe('Unknown (42)');
  });
});

describe('chargeDirection', () => {
  it('reports charging for active charger states regardless of current', () => {
    expect(chargeDirection(500, 1)).toBe('charging'); // trickle
    expect(chargeDirection(500, 3)).toBe('charging'); // fast CC
    expect(chargeDirection(50, 6)).toBe('charging'); // top-off
  });

  it('reports done for charge termination', () => {
    expect(chargeDirection(0, 7)).toBe('done');
  });

  it('uses the current sign when the charger is not charging', () => {
    expect(chargeDirection(-350, 0)).toBe('discharging');
    expect(chargeDirection(20, 0)).toBe('charging');
    expect(chargeDirection(0, 0)).toBe('idle');
  });
});

describe('formatters', () => {
  it('formats volts from millivolts', () => {
    expect(formatVolts(7914)).toBe('7.91 V');
    expect(formatVolts(8400)).toBe('8.40 V');
  });

  it('formats watts to two decimals', () => {
    expect(formatWatts(2.7685)).toBe('2.77 W');
    expect(formatWatts(0)).toBe('0.00 W');
  });
});
