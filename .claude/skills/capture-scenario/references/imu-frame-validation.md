# Validating the IMU coordinate frame on hardware

`fw/docs/imu-coordinate-frame.md` documents the BMI270 axes as worn, and its "Bench verification"
section records the measured result (2026-08-24, fw v3.4.0-stable). That doc deliberately carries the
*result* only; the method and its failure modes live here. Each trap below cost real session time.

**Capture.** Hold the `board` lock, then `capture start <seconds>` on the shell. The `.csv` sidecar
carries `I,ms,seq,ax,ay,az,gx,gy,gz` rows at 25 Hz, scaled ×1000 (mm/s², mrad/s); pull it off the USB
mass-storage volume read-only, identifying the disk by its `RGB-SG` SCSI string rather than a fixed
`/dev/sdX` (`.claude/skills/provision-device/references/nand-disk.md`). Do **not** reach for the
`mcp__serial__rgb_sunglasses_capture_scenario` MCP tool for this: it front-loads an AGC freeze via
`sound agc gain`/`freeze`, subcommands that exist only in `CONFIG_APP_AUDIO_DEBUG` builds (a stock
image like v3.4.0-stable does not have them), and aborts before recording anything. The AGC is
irrelevant to IMU work — the plain shell command is the right tool.

**Accelerometer.** Six static poses, each putting one axis up; the up-axis reads +1 g and the other
two ~0. Segment by detecting stationary plateaus in the data rather than slicing by wall clock — then
the operator needs no start cue and timing slop costs nothing. **Set the stillness threshold from
hand tremor, not from zero**: a hand-held pose runs 0.2–0.5 rad/s, so a 0.15 rad/s cutoff chops each
plateau into sub-minimum fragments and drops poses from the result entirely. That reads exactly like
the operator skipped them, and it is not what happened. 0.6 rad/s works.

**Gyro polarity.** Sweep briskly in the named direction, return slowly, repeat. The peak sign is then
unambiguous without full rotations, which the USB tether prevents anyway.

**Cross-check the gyro signs independently of operator execution.** For a rigid body a fixed world
vector obeys `d(a)/dt = −ω × a`, so the accelerometer's gravity reading predicts what the gyro must
report; correlate measured against predicted derivative. Two conditions gate a usable sample and
**both** produce a convincing false negative when missed:

- **ω must not be parallel to gravity**, or `ω × a = 0` and there is nothing to correlate. Yaw with
  the head upright is exactly that case — a session that only yaws upright learns **nothing** about X
  yet reports a cosine near zero, which looks like a refuted axis rather than an untested one.
  Include at least one X rotation with the glasses tipped; the pose-transition rotations already
  present in the accelerometer capture serve well.
- **Rate must be low enough for a central difference at 25 Hz.** Deliberate sweeps reach 13 rad/s —
  30° of rotation per sample — which aliases badly and drags the correlation down across every axis.
  Band-limit to roughly 1–5 rad/s.

Well-conditioned samples give median cosine ≈ 0.9. A value near 0 means the geometry was degenerate,
not that the axis is wrong.
