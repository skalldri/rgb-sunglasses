/**
 * Deterministic IMU providers for scenarios. All values in SI units
 * (m/s^2, rad/s), sampled by the host on the 25 Hz grid.
 */

import { ImuProvider, ImuSample } from "./providers";

type Vec3 = [number, number, number];

function lerp3(a: Vec3, b: Vec3, t: number): Vec3 {
  return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];
}

/** Linear ramp of accel/gyro between two endpoints over [startMs, endMs]. */
export class RampImuProvider implements ImuProvider {
  constructor(
    private readonly opts: {
      fromAccel: Vec3;
      toAccel: Vec3;
      fromGyro?: Vec3;
      toGyro?: Vec3;
      startMs: number;
      endMs: number;
    },
  ) {}

  sampleAt(tMs: number): ImuSample {
    const { startMs, endMs } = this.opts;
    const t = Math.max(0, Math.min(1, (tMs - startMs) / Math.max(1, endMs - startMs)));
    return {
      accel: lerp3(this.opts.fromAccel, this.opts.toAccel, t),
      gyro: lerp3(this.opts.fromGyro ?? [0, 0, 0], this.opts.toGyro ?? [0, 0, 0], t),
    };
  }
}

/** Sinusoidal motion on one axis (a nod / head-shake stand-in). */
export class SineImuProvider implements ImuProvider {
  constructor(
    private readonly opts: {
      channel: "accel" | "gyro";
      axis: 0 | 1 | 2;
      amplitude: number;
      hz: number;
      /** Constant base sample the sine is added onto. */
      base?: ImuSample;
    },
  ) {}

  sampleAt(tMs: number): ImuSample {
    const base = this.opts.base ?? { accel: [0, 0, 9.81], gyro: [0, 0, 0] };
    const accel: Vec3 = [...base.accel];
    const gyro: Vec3 = [...base.gyro];
    const v = this.opts.amplitude * Math.sin(2 * Math.PI * this.opts.hz * (tMs / 1000));
    if (this.opts.channel === "accel") {
      accel[this.opts.axis] += v;
    } else {
      gyro[this.opts.axis] += v;
    }
    return { accel, gyro };
  }
}

/** Piecewise-linear keyframes: [{atMs, accel, gyro}]; holds ends. */
export class KeyframeImuProvider implements ImuProvider {
  constructor(private readonly frames: { atMs: number; accel: Vec3; gyro?: Vec3 }[]) {
    if (frames.length === 0) {
      throw new Error("KeyframeImuProvider needs at least one keyframe");
    }
  }

  sampleAt(tMs: number): ImuSample {
    const frames = this.frames;
    if (tMs <= frames[0].atMs) {
      return { accel: frames[0].accel, gyro: frames[0].gyro ?? [0, 0, 0] };
    }
    for (let i = 1; i < frames.length; i++) {
      if (tMs <= frames[i].atMs) {
        const a = frames[i - 1];
        const b = frames[i];
        const t = (tMs - a.atMs) / Math.max(1, b.atMs - a.atMs);
        return {
          accel: lerp3(a.accel, b.accel, t),
          gyro: lerp3(a.gyro ?? [0, 0, 0], b.gyro ?? [0, 0, 0], t),
        };
      }
    }
    const last = frames[frames.length - 1];
    return { accel: last.accel, gyro: last.gyro ?? [0, 0, 0] };
  }
}
