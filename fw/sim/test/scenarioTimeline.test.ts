import { test } from "node:test";
import assert from "node:assert/strict";
import { TimelineRunner, parseScalarParamValue } from "../core/scenarioTimeline";
import { RgbxParamType } from "../core/abi";
import { SimHost } from "../core/host";
import { ScenarioEvent } from "../core/scenario";

/** Structural stand-in for the few SimHost members the runner touches —
 * spinning up a real host (worker + wasm) is host.integration.test.ts's job. */
function mockHost(params: { name: string; type: RgbxParamType }[]) {
  const calls: string[] = [];
  const host = {
    simTimeMs: 0,
    metadata: { params },
    paramIndexByName: (name: string) => params.findIndex((p) => p.name === name),
    setParam: (idx: number, value: number) => calls.push(`set ${idx}=${value}`),
    setParamF32: (idx: number, value: number) => calls.push(`f32 ${idx}=${value}`),
    setStringParam: (idx: number, value: string) => calls.push(`str ${idx}=${value}`),
    pressButton: (idx: number) => calls.push(`press ${idx}`),
  };
  return { host: host as unknown as SimHost, raw: host, calls };
}

test("parseScalarParamValue: decimal, hex, booleans, and non-scalars", () => {
  assert.equal(parseScalarParamValue(7), 7);
  assert.equal(parseScalarParamValue("42"), 42);
  assert.equal(parseScalarParamValue("0x10"), 16);
  assert.equal(parseScalarParamValue("true"), 1);
  assert.equal(parseScalarParamValue("false"), 0);
  assert.equal(parseScalarParamValue("banana"), null);
  assert.equal(parseScalarParamValue("-3"), null);
});

test("events fire in atMs order, each exactly once, when simTime covers them", () => {
  const { host, raw, calls } = mockHost([
    { name: "speed", type: RgbxParamType.Uint32 },
    { name: "label", type: RgbxParamType.String },
  ]);
  // Deliberately out of order in the file; the runner sorts.
  const events: ScenarioEvent[] = [
    { atMs: 200, press: "Down" },
    { atMs: 0, set: { speed: 5 } },
    { atMs: 100, set: { label: "hi" } },
  ];
  const runner = new TimelineRunner(events);

  assert.deepEqual(runner.pump(host), { firedSet: true, firedPress: false });
  assert.deepEqual(calls, ["set 0=5"]);

  raw.simTimeMs = 99;
  assert.deepEqual(runner.pump(host), { firedSet: false, firedPress: false });

  raw.simTimeMs = 250;
  assert.deepEqual(runner.pump(host), { firedSet: true, firedPress: true });
  assert.deepEqual(calls, ["set 0=5", "str 1=hi", "press 3"]);

  // Nothing left; pumping again is a no-op.
  assert.deepEqual(runner.pump(host), { firedSet: false, firedPress: false });
  assert.equal(calls.length, 3);
});

test("a bad param name throws with the timeline timestamp", () => {
  const { host } = mockHost([{ name: "speed", type: RgbxParamType.Uint32 }]);
  const runner = new TimelineRunner([{ atMs: 0, set: { nope: 1 } }]);
  assert.throws(() => runner.pump(host), /timeline @0ms: no param named "nope"/);
});

test("a STRING param takes scalar-shaped tokens verbatim", () => {
  const { host, calls } = mockHost([{ name: "label", type: RgbxParamType.String }]);
  new TimelineRunner([{ atMs: 0, set: { label: "true" } }]).pump(host);
  assert.deepEqual(calls, ["str 0=true"]);
});

test("a FLOAT param routes through setParamF32, never the truncating setParam", () => {
  // Regression guard for the `>>> 0` trap: setParam would turn 0.5 into 0.
  const { host, calls } = mockHost([{ name: "gain", type: RgbxParamType.Float }]);
  new TimelineRunner([
    { atMs: 0, set: { gain: 0.5 } },
    { atMs: 0, set: { gain: "1.25" } },
  ]).pump(host);
  assert.deepEqual(calls, ["f32 0=0.5", "f32 0=1.25"]);
});

test("a FLOAT param rejects non-finite and non-numeric tokens", () => {
  const { host } = mockHost([{ name: "gain", type: RgbxParamType.Float }]);
  assert.throws(
    () => new TimelineRunner([{ atMs: 0, set: { gain: "banana" } }]).pump(host),
    /expects a finite float/,
  );
  assert.throws(
    () => new TimelineRunner([{ atMs: 0, set: { gain: "Infinity" } }]).pump(host),
    /expects a finite float/,
  );
});

test("a decimal on a non-FLOAT param stays an error, not a truncation", () => {
  const { host } = mockHost([{ name: "speed", type: RgbxParamType.Uint32 }]);
  assert.throws(
    () => new TimelineRunner([{ atMs: 0, set: { speed: "0.5" } }]).pump(host),
    /expects a number/,
  );
});
