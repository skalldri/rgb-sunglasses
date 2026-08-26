/**
 * Tests for the telemetry subscription lifecycle and the render-isolation design.
 *
 * The lifecycle assertions here encode a firmware contract that is easy to get backwards and
 * expensive to debug on hardware: the app MUST subscribe before writing a non-zero tier, or
 * the firmware rejects the write with -EACCES. Nothing in the type system enforces that
 * ordering, so it is enforced here.
 *
 * The isolation test is the one that cannot be replaced by looking at the code. It pushes a
 * stream of frames and asserts a consumer that reads no telemetry never re-renders — which is
 * the entire architectural claim of context/audio-telemetry-context.tsx.
 */

import { act, render } from "@testing-library/react-native";
import React from "react";
import { Text } from "react-native";

const focusState = { focused: true };
jest.mock("expo-router", () => {
  // eslint-disable-next-line @typescript-eslint/no-require-imports -- mock factories cannot close over imports
  const ReactActual = require("react");
  return {
    useFocusEffect: (cb: () => void | (() => void)) => {
      ReactActual.useEffect(() => {
        if (!focusState.focused) return undefined;
        return cb();
      }, [cb, focusState.focused]);
    },
  };
});

import {
  UUID_AUDIO_TELEMETRY,
  UUID_AUDIO_TELEMETRY_SERVICE,
  UUID_TELEMETRY_CONTROL,
} from "@/constants/bluetooth";
import * as BluetoothContext from "@/context/bluetooth-context";
import {
  AudioTelemetryProvider,
  REQUESTED_HOLD_S,
  REQUESTED_RATE_HZ,
  REQUESTED_TIER,
  encodeControl,
  useAudioTelemetry as useAudioTelemetryContextForTest,
  useAudioTelemetryStatus,
  useAudioTelemetrySummary,
} from "@/context/audio-telemetry-context";
import { TELEMETRY_TIER_OFF } from "@/services/audio-telemetry";
import { makeFrame, toBase64 } from "./fixtures/audio-telemetry";

type MonitorCb = (
  error: { message?: string } | null,
  characteristic: { value?: string | null } | null,
) => void;

function buildDevice(opts: { withTelemetry?: boolean } = {}) {
  const monitorCbs: MonitorCb[] = [];
  const remove = jest.fn();
  const monitor = jest.fn((cb: MonitorCb) => {
    monitorCbs.push(cb);
    return { remove };
  });
  const writeWithResponse = jest.fn().mockResolvedValue(true);

  const byService: Record<string, any> = {};
  if (opts.withTelemetry !== false) {
    byService[UUID_AUDIO_TELEMETRY_SERVICE] = {
      [UUID_TELEMETRY_CONTROL]: { characteristic: { writeWithResponse } },
      [UUID_AUDIO_TELEMETRY]: {
        characteristic: { monitor, isNotifiable: true },
      },
    };
  }
  return {
    device: {
      name: "RGB Sunglasses",
      mac: "AA:BB:CC",
      characteristicsByService: byService,
      characteristics: {},
    },
    monitorCbs,
    monitor,
    remove,
    writeWithResponse,
    emit(bytes: Uint8Array) {
      act(() => {
        monitorCbs.forEach((cb) => cb(null, { value: toBase64(bytes) }));
      });
    },
  };
}

function mockBluetooth(device: any) {
  jest
    .spyOn(BluetoothContext, "useBluetooth")
    .mockImplementation(() => ({ selectedDevice: device }) as any);
}

function Probe() {
  const summary = useAudioTelemetrySummary();
  const status = useAudioTelemetryStatus();
  return (
    <Text testID="probe">{`${status}:${summary.frames}:${summary.beatsPerSecond.toFixed(1)}`}</Text>
  );
}

describe("AudioTelemetryProvider", () => {
  beforeEach(() => {
    focusState.focused = true;
    jest.useFakeTimers();
    jest.spyOn(console, "log").mockImplementation(() => {});
    jest.spyOn(console, "error").mockImplementation(() => {});
  });
  afterEach(() => {
    jest.runOnlyPendingTimers();
    jest.useRealTimers();
    jest.restoreAllMocks();
  });

  describe("arming", () => {
    it("subscribes BEFORE writing a non-zero tier", () => {
      // The firmware rejects arm-before-subscribe with -EACCES, deliberately: ATT serializes
      // on one bearer, so a control-first write cannot get its CCCD write in ahead of the
      // first tick, and that tick would kill an ACKed stream with nothing left to re-arm it.
      const h = buildDevice();
      mockBluetooth(h.device);
      render(
        <AudioTelemetryProvider>
          <Probe />
        </AudioTelemetryProvider>,
      );

      expect(h.monitor).toHaveBeenCalledTimes(1);
      expect(h.writeWithResponse).toHaveBeenCalledTimes(1);
      expect(h.monitor.mock.invocationCallOrder[0]).toBeLessThan(
        h.writeWithResponse.mock.invocationCallOrder[0],
      );
    });

    it("arms with the requested tier, rate and hold", () => {
      const h = buildDevice();
      mockBluetooth(h.device);
      render(
        <AudioTelemetryProvider>
          <Probe />
        </AudioTelemetryProvider>,
      );
      expect(h.writeWithResponse).toHaveBeenCalledWith(
        encodeControl(REQUESTED_TIER, REQUESTED_RATE_HZ, REQUESTED_HOLD_S),
      );
    });

    it("re-arms at half the hold, so the firmware watchdog never expires while focused", () => {
      // The watchdog exists so a backgrounded phone cannot leave the device notifying into a
      // void until the battery dies. Re-arming is this side of that contract.
      const h = buildDevice();
      mockBluetooth(h.device);
      render(
        <AudioTelemetryProvider>
          <Probe />
        </AudioTelemetryProvider>,
      );
      expect(h.writeWithResponse).toHaveBeenCalledTimes(1);

      act(() => {
        jest.advanceTimersByTime((REQUESTED_HOLD_S / 2) * 1000 + 10);
      });
      expect(h.writeWithResponse).toHaveBeenCalledTimes(2);

      act(() => {
        jest.advanceTimersByTime((REQUESTED_HOLD_S / 2) * 1000);
      });
      expect(h.writeWithResponse).toHaveBeenCalledTimes(3);
      // Every re-arm is the same word — the firmware treats a repeat as a hold extension.
      expect(h.writeWithResponse).toHaveBeenLastCalledWith(
        encodeControl(REQUESTED_TIER, REQUESTED_RATE_HZ, REQUESTED_HOLD_S),
      );
    });

    it("reports unsupported on firmware without the service, without writing anything", () => {
      const h = buildDevice({ withTelemetry: false });
      mockBluetooth(h.device);
      const { getByTestId } = render(
        <AudioTelemetryProvider>
          <Probe />
        </AudioTelemetryProvider>,
      );
      expect(getByTestId("probe").props.children).toContain("unsupported");
      expect(h.monitor).not.toHaveBeenCalled();
      expect(h.writeWithResponse).not.toHaveBeenCalled();
    });
  });

  describe("teardown", () => {
    it("stops the stream and unsubscribes on blur", () => {
      const h = buildDevice();
      mockBluetooth(h.device);
      const { rerender } = render(
        <AudioTelemetryProvider>
          <Probe />
        </AudioTelemetryProvider>,
      );
      h.writeWithResponse.mockClear();

      act(() => {
        focusState.focused = false;
        rerender(
          <AudioTelemetryProvider>
            <Probe />
          </AudioTelemetryProvider>,
        );
      });

      expect(h.writeWithResponse).toHaveBeenCalledWith(
        encodeControl(TELEMETRY_TIER_OFF, 0, 0),
      );
      expect(h.remove).toHaveBeenCalledTimes(1);
    });

    it("stops re-arming after blur", () => {
      const h = buildDevice();
      mockBluetooth(h.device);
      const { rerender } = render(
        <AudioTelemetryProvider>
          <Probe />
        </AudioTelemetryProvider>,
      );
      act(() => {
        focusState.focused = false;
        rerender(
          <AudioTelemetryProvider>
            <Probe />
          </AudioTelemetryProvider>,
        );
      });
      h.writeWithResponse.mockClear();
      act(() => {
        jest.advanceTimersByTime(REQUESTED_HOLD_S * 1000 * 3);
      });
      expect(h.writeWithResponse).not.toHaveBeenCalled();
    });

    it("ignores frames delivered after teardown", () => {
      // rxandroidble tears a notification down fire-and-forget, so a late callback is normal.
      const h = buildDevice();
      mockBluetooth(h.device);
      const { rerender, getByTestId } = render(
        <AudioTelemetryProvider>
          <Probe />
        </AudioTelemetryProvider>,
      );
      act(() => {
        focusState.focused = false;
        rerender(
          <AudioTelemetryProvider>
            <Probe />
          </AudioTelemetryProvider>,
        );
      });
      h.emit(makeFrame({ beatMask: 0x1 }));
      act(() => {
        jest.advanceTimersByTime(1000);
      });
      expect(getByTestId("probe").props.children).toContain("idle");
    });
  });

  describe("frames", () => {
    it("reports streaming once a frame arrives, and summarises at 2 Hz", () => {
      const h = buildDevice();
      mockBluetooth(h.device);
      const { getByTestId } = render(
        <AudioTelemetryProvider>
          <Probe />
        </AudioTelemetryProvider>,
      );

      for (let n = 0; n < 8; n++) {
        h.emit(makeFrame({ seq: n * 4, beatMask: n % 4 === 0 ? 0x1 : 0 }));
      }
      // Frames alone must not re-render — the summary only exists after its tick.
      expect(getByTestId("probe").props.children).toContain(":0:");

      act(() => {
        jest.advanceTimersByTime(600);
      });
      const text = getByTestId("probe").props.children as string;
      expect(text).toContain("streaming");
      expect(text).toContain(":8:");
    });

    it("discards a frame whose version it does not recognise", () => {
      const h = buildDevice();
      mockBluetooth(h.device);
      const { getByTestId } = render(
        <AudioTelemetryProvider>
          <Probe />
        </AudioTelemetryProvider>,
      );
      for (let n = 0; n < 5; n++) h.emit(makeFrame({ version: 7 }));
      act(() => {
        jest.advanceTimersByTime(600);
      });
      expect(getByTestId("probe").props.children).toContain(":0:");
    });

    it("treats cancel and disconnect errors as a normal end, not a failure", () => {
      const h = buildDevice();
      mockBluetooth(h.device);
      const { getByTestId } = render(
        <AudioTelemetryProvider>
          <Probe />
        </AudioTelemetryProvider>,
      );
      act(() => {
        h.monitorCbs.forEach((cb) =>
          cb({ message: "Operation was cancelled" }, null),
        );
        h.monitorCbs.forEach((cb) =>
          cb({ message: "Device was disconnected" }, null),
        );
      });
      act(() => {
        jest.advanceTimersByTime(600);
      });
      expect(getByTestId("probe").props.children).not.toContain("error");
      expect(console.error).not.toHaveBeenCalled();
    });

    it("surfaces a real monitor error", () => {
      const h = buildDevice();
      mockBluetooth(h.device);
      const { getByTestId } = render(
        <AudioTelemetryProvider>
          <Probe />
        </AudioTelemetryProvider>,
      );
      act(() => {
        h.monitorCbs.forEach((cb) =>
          cb({ message: "GATT_INSUFFICIENT_ENCRYPTION" }, null),
        );
      });
      act(() => {
        jest.advanceTimersByTime(600);
      });
      expect(getByTestId("probe").props.children).toContain("error");
    });
  });

  describe("render isolation", () => {
    // These are the architectural claims of context/audio-telemetry-context.tsx, and they are
    // easy to assert vacuously. Two earlier versions of these tests passed against a provider
    // deliberately broken to re-render per frame: `children` passed as a prop are immune by
    // construction (React reuses the element), and notifying a useSyncExternalStore listener
    // without changing the snapshot cannot re-render anything. Both tests below were checked
    // against a mutant that genuinely breaks the property they describe.

    it("keeps the context value identity stable across frames", () => {
      // The claim: consumers holding the telemetry context never re-render because of it.
      // The mechanism is the EMPTY-deps useMemo; drop it and this fails.
      const identities: unknown[] = [];
      function Consumer() {
        identities.push(useAudioTelemetryContextForTest());
        return <Text testID="c">c</Text>;
      }
      const h = buildDevice();
      mockBluetooth(h.device);
      const { rerender } = render(
        <AudioTelemetryProvider>
          <Consumer />
        </AudioTelemetryProvider>,
      );

      for (let n = 0; n < 100; n++) {
        h.emit(
          makeFrame({ seq: n * 4, tier: 3, beatMask: n % 4 === 0 ? 0x1 : 0 }),
        );
      }
      act(() => {
        jest.advanceTimersByTime(2000);
      });
      // Force the provider to re-render for reasons unrelated to telemetry.
      act(() => {
        rerender(
          <AudioTelemetryProvider>
            <Consumer />
          </AudioTelemetryProvider>,
        );
      });

      expect(identities.length).toBeGreaterThan(0);
      expect(new Set(identities).size).toBe(1);
    });

    it("recomputes the summary on the tick, not per frame", () => {
      // 50 frames must cost one summary render, not fifty. summarizeTelemetry() returns a
      // fresh object every call, so moving it into the frame path would make each frame a
      // new snapshot and re-render every consumer -- which is the mutant this test kills.
      const seen: number[] = [];
      function Watcher() {
        const s = useAudioTelemetrySummary();
        seen.push(s.frames);
        return <Text testID="watch">w</Text>;
      }
      const h = buildDevice();
      mockBluetooth(h.device);
      render(
        <AudioTelemetryProvider>
          <Watcher />
        </AudioTelemetryProvider>,
      );
      const baseline = seen.length;

      for (let n = 0; n < 50; n++) h.emit(makeFrame({ seq: n * 4 }));
      expect(seen.length).toBe(baseline);

      act(() => {
        jest.advanceTimersByTime(600);
      });
      expect(seen.length).toBe(baseline + 1);
      expect(seen[seen.length - 1]).toBe(50);

      act(() => {
        jest.advanceTimersByTime(600);
      });
      expect(seen.length).toBe(baseline + 2);
    });
  });

  describe("recovery paths from review", () => {
    it("keeps retrying while the device is still discovering, instead of latching unsupported", () => {
      // Discovery is ~170 sequential GATT reads and only populates selectedDevice at the end, so
      // a single 1.5 s retry could easily land before it finishes. When it did, the screen told
      // the user their current firmware was too old — permanently, with no path back except
      // blurring and refocusing.
      const h = buildDevice();
      const late = { ...h.device, characteristicsByService: undefined };
      jest
        .spyOn(BluetoothContext, "useBluetooth")
        .mockImplementation(() => ({ selectedDevice: late }) as any);

      const { getByTestId } = render(
        <AudioTelemetryProvider>
          <Probe />
        </AudioTelemetryProvider>,
      );

      act(() => {
        jest.advanceTimersByTime(5000);
      });
      expect(getByTestId("probe").props.children).not.toContain("unsupported");

      // Discovery completes: the retry must pick it up without a refocus.
      jest
        .spyOn(BluetoothContext, "useBluetooth")
        .mockImplementation(() => ({ selectedDevice: h.device }) as any);
      (late as any).characteristicsByService =
        h.device.characteristicsByService;
      act(() => {
        jest.advanceTimersByTime(2000);
      });
      expect(h.monitor).toHaveBeenCalled();
    });

    it("stops the re-arm timer on a link error instead of streaming into nothing", () => {
      // The re-arm interval kept extending the firmware's watchdog hold over a dead
      // subscription: the device carried on encoding and notifying at 8-32 Hz to nobody, and
      // the governor kept holding the faster connection interval — the exact battery cost the
      // stream-hold design exists to avoid.
      const h = buildDevice();
      mockBluetooth(h.device);
      render(
        <AudioTelemetryProvider>
          <Probe />
        </AudioTelemetryProvider>,
      );
      h.writeWithResponse.mockClear();

      act(() => {
        h.monitorCbs.forEach((cb) =>
          cb({ message: "GATT_INSUFFICIENT_ENCRYPTION" }, null),
        );
      });
      act(() => {
        jest.advanceTimersByTime(REQUESTED_HOLD_S * 1000 * 3);
      });

      const rearms = h.writeWithResponse.mock.calls.filter(
        (c) =>
          c[0] ===
          encodeControl(REQUESTED_TIER, REQUESTED_RATE_HZ, REQUESTED_HOLD_S),
      );
      expect(rearms).toHaveLength(0);
    });

    it("re-arms after a disconnect-shaped error once the link is usable again", () => {
      // Disconnect errors were filtered as "normal end" with no recovery, so the meters stayed
      // dead after a mid-focus link drop even once the link came back.
      const h = buildDevice();
      mockBluetooth(h.device);
      render(
        <AudioTelemetryProvider>
          <Probe />
        </AudioTelemetryProvider>,
      );
      expect(h.monitor).toHaveBeenCalledTimes(1);

      act(() => {
        h.monitorCbs.forEach((cb) =>
          cb({ message: "Device was disconnected" }, null),
        );
      });
      act(() => {
        jest.advanceTimersByTime(4000);
      });
      expect(h.monitor.mock.calls.length).toBeGreaterThan(1);
    });
  });
});
