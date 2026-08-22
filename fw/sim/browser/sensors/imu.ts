/**
 * IMU sources for the browser UI. Each is a plain ImuProvider assigned to
 * SimHost.imuProvider; the host already handles the 25 Hz sample-and-hold,
 * so sampleAt() just reports whatever the source currently believes.
 *
 * Units follow the rgbx ABI (and the BMI270 pipeline behind it): accel in
 * m/s^2 including gravity, gyro in rad/s.
 */

import type { ImuProvider, ImuSample } from "../../core/providers";

/**
 * Pass-through provider that remembers the last sample it handed to the
 * host, so the readout shows what the extension saw regardless of which
 * source produced it (manual or scenario). Indirects through a getter —
 * the same pattern as TappedAudioProvider — so it can be assigned to
 * SimHost.imuProvider once and the source swapped underneath.
 */
export class TappedImuProvider implements ImuProvider {
  last: ImuSample = { accel: [0, 0, 9.81], gyro: [0, 0, 0] };

  constructor(private readonly inner: () => ImuProvider) {}

  sampleAt(tMs: number): ImuSample {
    const sample = this.inner().sampleAt(tMs);
    this.last = sample;
    return sample;
  }
}

export const G = 9.81;
const DEG_TO_RAD = Math.PI / 180;

export type ImuSourceKind = "static" | "mouse" | "orientation" | "motion";

/** Hand-set constant orientation — the default, matching StaticImuProvider. */
export class StaticImuSource implements ImuProvider {
  accel: [number, number, number] = [0, 0, G];
  gyro: [number, number, number] = [0, 0, 0];

  sampleAt(): ImuSample {
    return { accel: [...this.accel], gyro: [...this.gyro] };
  }
}

/**
 * Drag anywhere on the glasses canvas to tilt the virtual device: the drag
 * position maps to a tilt angle (up to MAX_TILT from flat) and the gravity
 * vector is rotated accordingly, so |accel| always stays at 1 g. Gyro is the
 * finite difference of the tilt angles between IMU samples, which is what a
 * real gyro would report for the same motion.
 */
export class MouseTiltImuSource implements ImuProvider {
  private static readonly MAX_TILT = (60 * Math.PI) / 180;

  /** Normalized drag position in [-1, 1]; (0, 0) is flat/screen-up. */
  private nx = 0;
  private ny = 0;
  private lastTiltX = 0;
  private lastTiltY = 0;
  private lastTimeMs = 0;

  setPoint(nx: number, ny: number): void {
    this.nx = nx;
    this.ny = ny;
  }

  recenter(): void {
    this.nx = 0;
    this.ny = 0;
  }

  sampleAt(tMs: number): ImuSample {
    const tiltX = this.nx * MouseTiltImuSource.MAX_TILT;
    const tiltY = -this.ny * MouseTiltImuSource.MAX_TILT;
    const ax = G * Math.sin(tiltX);
    const ay = G * Math.sin(tiltY);
    const az = Math.sqrt(Math.max(0, G * G - ax * ax - ay * ay));

    const dt = (tMs - this.lastTimeMs) / 1000;
    const gyro: [number, number, number] =
      dt > 0 ? [(tiltY - this.lastTiltY) / dt, (tiltX - this.lastTiltX) / dt, 0] : [0, 0, 0];
    this.lastTiltX = tiltX;
    this.lastTiltY = tiltY;
    this.lastTimeMs = tMs;

    return { accel: [ax, ay, az], gyro };
  }
}

/**
 * DeviceOrientationEvent: beta/gamma give the device's tilt, from which the
 * gravity vector is reconstructed. There is no rotation-rate information in
 * this event, so gyro is differentiated from the angles like the mouse
 * source does. Flat and screen-up reads (0, 0, +g), matching the
 * accelerationIncludingGravity convention of DeviceMotion.
 */
export class DeviceOrientationImuSource implements ImuProvider {
  private beta = 0;
  private gamma = 0;
  private lastBeta = 0;
  private lastGamma = 0;
  private lastTimeMs = 0;
  private readonly handler = (ev: DeviceOrientationEvent) => {
    this.beta = ev.beta ?? 0;
    this.gamma = ev.gamma ?? 0;
  };

  start(): void {
    window.addEventListener("deviceorientation", this.handler);
  }

  stop(): void {
    window.removeEventListener("deviceorientation", this.handler);
  }

  sampleAt(tMs: number): ImuSample {
    const b = this.beta * DEG_TO_RAD;
    const g = this.gamma * DEG_TO_RAD;
    const accel: [number, number, number] = [
      -G * Math.sin(g),
      G * Math.sin(b) * Math.cos(g),
      G * Math.cos(b) * Math.cos(g),
    ];
    const dt = (tMs - this.lastTimeMs) / 1000;
    const gyro: [number, number, number] =
      dt > 0
        ? [
            ((this.beta - this.lastBeta) * DEG_TO_RAD) / dt,
            ((this.gamma - this.lastGamma) * DEG_TO_RAD) / dt,
            0,
          ]
        : [0, 0, 0];
    this.lastBeta = this.beta;
    this.lastGamma = this.gamma;
    this.lastTimeMs = tMs;
    return { accel, gyro };
  }
}

/** DeviceMotionEvent — the closest thing a phone browser has to the real
 * sensor: gravity-inclusive acceleration in m/s^2 and rotation rate in
 * deg/s, converted to rad/s. */
export class DeviceMotionImuSource implements ImuProvider {
  private accel: [number, number, number] = [0, 0, G];
  private gyro: [number, number, number] = [0, 0, 0];
  private readonly handler = (ev: DeviceMotionEvent) => {
    const a = ev.accelerationIncludingGravity;
    if (a !== null) {
      this.accel = [a.x ?? 0, a.y ?? 0, a.z ?? 0];
    }
    const r = ev.rotationRate;
    if (r !== null) {
      this.gyro = [
        (r.beta ?? 0) * DEG_TO_RAD,
        (r.gamma ?? 0) * DEG_TO_RAD,
        (r.alpha ?? 0) * DEG_TO_RAD,
      ];
    }
  };

  start(): void {
    window.addEventListener("devicemotion", this.handler);
  }

  stop(): void {
    window.removeEventListener("devicemotion", this.handler);
  }

  sampleAt(): ImuSample {
    return { accel: [...this.accel], gyro: [...this.gyro] };
  }
}

/**
 * Multiplexes the four sources behind one ImuProvider, so it can be assigned
 * to SimHost.imuProvider once and switched underneath. Also records the last
 * sample delivered for the UI readout — the host samples on the 40 ms grid
 * and holds between, so this is exactly what the extension saw.
 */
export class ImuManager implements ImuProvider {
  readonly staticSource = new StaticImuSource();
  readonly mouse = new MouseTiltImuSource();
  readonly orientation = new DeviceOrientationImuSource();
  readonly motion = new DeviceMotionImuSource();

  private kind: ImuSourceKind = "static";
  last: ImuSample = { accel: [0, 0, G], gyro: [0, 0, 0] };

  get sourceKind(): ImuSourceKind {
    return this.kind;
  }

  setKind(kind: ImuSourceKind): void {
    if (kind === this.kind) {
      return;
    }
    // Only the listening sources need starting/stopping; leaving a listener
    // attached would keep firing events for a source nobody is reading.
    this.orientation.stop();
    this.motion.stop();
    this.kind = kind;
    if (kind === "orientation") {
      this.orientation.start();
    } else if (kind === "motion") {
      this.motion.start();
    }
  }

  sampleAt(tMs: number): ImuSample {
    const sample = this.active().sampleAt(tMs);
    this.last = sample;
    return sample;
  }

  private active(): ImuProvider {
    switch (this.kind) {
      case "mouse": return this.mouse;
      case "orientation": return this.orientation;
      case "motion": return this.motion;
      default: return this.staticSource;
    }
  }
}

/** iOS 13+ gates both motion events behind a user-gesture permission call.
 * Returns true when the events may be used (including on platforms with no
 * permission model at all). */
export async function requestMotionPermission(): Promise<boolean> {
  type Gated = { requestPermission?: () => Promise<string> };
  const ctors: Gated[] = [];
  if (typeof DeviceMotionEvent !== "undefined") {
    ctors.push(DeviceMotionEvent as unknown as Gated);
  }
  if (typeof DeviceOrientationEvent !== "undefined") {
    ctors.push(DeviceOrientationEvent as unknown as Gated);
  }

  for (const ctor of ctors) {
    // Absent requestPermission = no permission model (Android/desktop): the
    // events just work. Present = call it as a METHOD, Safari's
    // implementation needs its `this`.
    if (typeof ctor.requestPermission !== "function") {
      continue;
    }
    if ((await ctor.requestPermission()) !== "granted") {
      return false;
    }
  }
  return true;
}
