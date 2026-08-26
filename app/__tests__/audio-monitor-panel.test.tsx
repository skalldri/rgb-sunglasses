/**
 * Tests for the monitor panel's degraded states.
 *
 * These are the branches that tell the user something about their CONNECTION rather than
 * their audio, so a wrong one sends them off to fix a problem they do not have. Hardware-found
 * 2026-08-25: before any frame arrived the panel claimed the link was running at its smallest
 * packet size, when in truth nothing had been decoded at all.
 */

import { render } from "@testing-library/react-native";
import { StyleSheet } from "react-native";
import React from "react";

const focusState = { focused: true };
jest.mock("expo-router", () => {
  // eslint-disable-next-line @typescript-eslint/no-require-imports -- mock factories cannot close over imports
  const ReactActual = require("react");
  return {
    useFocusEffect: (cb: () => void | (() => void)) => {
      ReactActual.useEffect(
        () => (focusState.focused ? cb() : undefined),
        [cb],
      );
    },
  };
});

import { MonitorPanel } from "@/components/audio/monitor-panel";
import * as TelemetryContext from "@/context/audio-telemetry-context";
import {
  EMPTY_SUMMARY,
  TELEMETRY_TIER_METERS,
  TELEMETRY_TIER_STATS,
  TELEMETRY_TIER_SPECTRUM,
  createTelemetryRing,
  type TelemetrySummary,
} from "@/services/audio-telemetry";

function mockTelemetry(
  summary: TelemetrySummary,
  status: TelemetryContext.TelemetryStatus,
) {
  const ring = { current: createTelemetryRing(8) };
  jest
    .spyOn(TelemetryContext, "useAudioTelemetrySummary")
    .mockReturnValue(summary);
  jest
    .spyOn(TelemetryContext, "useAudioTelemetryStatus")
    .mockReturnValue(status);
  jest.spyOn(TelemetryContext, "useAudioTelemetry").mockReturnValue({
    ring,
    shared: {
      rmsInputDb: { value: -40 },
      peakDb: { value: -12 },
      noiseFloorDb: { value: -64 },
      gainDb: { value: 6 },
      bandRatio: [0, 1, 2, 3].map(() => ({ value: 0.5 })),
      buckets: Array.from({ length: 20 }, () => ({ value: 0.3 })),
      beatTick: { value: 0 },
      beatBand: { value: 0 },
      liveness: { value: 1 },
    },
    subscribeSummary: () => () => {},
    getSummarySnapshot: () => summary,
    getStatus: () => status,
    subscribeStatus: () => () => {},
    requestStream: jest.fn(),
  } as unknown as TelemetryContext.AudioTelemetryContextValue);
}

const PROPS = { targetLow: 0.002, targetHigh: 0.05, noiseGate: 0.0006 };

describe("MonitorPanel spectrum states", () => {
  afterEach(() => jest.restoreAllMocks());

  it("does not blame the link before any frame has arrived", () => {
    // The bug: tier is OFF until the first frame decodes, which is NOT the same as "this link
    // cannot carry a spectrum". Claiming a degraded link here sends someone to re-pair a
    // connection that is perfectly fine.
    mockTelemetry(EMPTY_SUMMARY, "starting");
    const { queryByTestId } = render(<MonitorPanel {...PROPS} />);
    expect(queryByTestId("audio-monitor-no-spectrum")).toBeNull();
    expect(queryByTestId("spectrum-bars")).not.toBeNull();
  });

  it("blames the link only when frames ARE arriving at a lower tier", () => {
    // This is the real MTU-23 case: the firmware clamped the tier it could send, and the app
    // should say so and point at re-pairing.
    mockTelemetry(
      { ...EMPTY_SUMMARY, frames: 40, live: true, tier: TELEMETRY_TIER_METERS },
      "streaming",
    );
    const { getByTestId } = render(<MonitorPanel {...PROPS} />);
    expect(getByTestId("audio-monitor-no-spectrum").props.children).toContain(
      "smallest packet size",
    );
  });

  it("shows the spectrum when the full tier is arriving", () => {
    mockTelemetry(
      {
        ...EMPTY_SUMMARY,
        frames: 40,
        live: true,
        tier: TELEMETRY_TIER_SPECTRUM,
      },
      "streaming",
    );
    const { queryByTestId } = render(<MonitorPanel {...PROPS} />);
    expect(queryByTestId("audio-monitor-no-spectrum")).toBeNull();
    expect(queryByTestId("spectrum-bars")).not.toBeNull();
  });

  it("tells the user to update firmware when the service is absent", () => {
    mockTelemetry(EMPTY_SUMMARY, "unsupported");
    const { getByTestId } = render(<MonitorPanel {...PROPS} />);
    expect(getByTestId("audio-monitor-unsupported")).not.toBeNull();
  });
});

describe("MonitorPanel downgrade copy", () => {
  afterEach(() => jest.restoreAllMocks());

  it("prescribes re-pairing only for the genuinely MTU-limited tier", () => {
    // Tier 1 is the 20-byte tier that exists to survive an unnegotiated ATT MTU of 23, so
    // seeing it really is evidence of a degraded link.
    mockTelemetry(
      { ...EMPTY_SUMMARY, frames: 40, live: true, tier: TELEMETRY_TIER_METERS },
      "streaming",
    );
    const { getByTestId } = render(<MonitorPanel {...PROPS} />);
    expect(getByTestId("audio-monitor-no-spectrum").props.children).toContain(
      "smallest packet size",
    );
  });

  it("does not blame the link for a deliberately reduced tier", () => {
    // Tier 2 is 28 bytes — it fits every MTU this stack negotiates, so it only appears
    // because something ASKED for it. The calibration wizard does exactly that for its
    // tap-along step, which would otherwise tell every wizard user to go re-pair.
    mockTelemetry(
      { ...EMPTY_SUMMARY, frames: 40, live: true, tier: TELEMETRY_TIER_STATS },
      "streaming",
    );
    const { getByTestId } = render(<MonitorPanel {...PROPS} />);
    const copy = getByTestId("audio-monitor-no-spectrum").props.children;
    expect(copy).not.toContain("smallest packet size");
    expect(copy).not.toContain("Re-pairing");
  });
});

describe("MonitorPanel header layout", () => {
  afterEach(() => jest.restoreAllMocks());

  /* WHAT THIS CAN AND CANNOT PROVE.
   *
   * jest-expo has no layout engine, so nothing here can observe the actual overflow — the
   * evidence for the bug is a device screenshot (2026-08-26: the green pill rendered as "LIV"
   * with its right half past the screen edge, because BeatPulse's `flex: 1` inner text sized
   * the header's first child to the width it WANTED and pushed the pill out of the card).
   *
   * What this DOES do is pin the two constraints that fix it, so removing either is a test
   * failure rather than a silent regression only a screenshot would catch. Treat a green run
   * as "the constraint is still declared", never as "the header fits".
   */
  it("keeps the status pill from being squeezed out of the header", () => {
    mockTelemetry(EMPTY_SUMMARY, "streaming");
    const { getByTestId } = render(<MonitorPanel {...PROPS} />);

    const pill = getByTestId("audio-monitor-pill");
    const flat = StyleSheet.flatten(pill.props.style) as Record<string, unknown>;
    expect(flat.flexShrink).toBe(0);
  });
});
