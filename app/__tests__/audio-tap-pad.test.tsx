import { fireEvent, render } from "@testing-library/react-native";
import React from "react";

import { TapPad } from "@/components/audio/tap-pad";

jest.mock("expo-haptics", () => ({
  impactAsync: jest.fn().mockResolvedValue(undefined),
  ImpactFeedbackStyle: { Light: "light" },
}));

/**
 * Wiring-level coverage for the pad's actual Pressable.
 *
 * The hook suite calls recordTap() directly, which proved nothing about the component the
 * finger touches — a blind spot found on hardware (2026-08-29): a screen-level failure made
 * the pad show press feedback while no tap was ever counted, and every test stayed green.
 * fireEvent.press only exercises the handler wiring, not the native gesture responder — the
 * responder layer is hardware-verified (80/80 real taps counted that session) — but this at
 * least fails if the Pressable ever stops delivering onPress to onTap.
 */
describe("TapPad", () => {
  it("delivers a press on the real Pressable to onTap", () => {
    const onTap = jest.fn();
    const { getByTestId } = render(
      <TapPad count={0} minimum={8} onTap={onTap} />,
    );
    fireEvent.press(getByTestId("tap-pad"));
    expect(onTap).toHaveBeenCalledTimes(1);
  });

  it("ignores presses while disabled", () => {
    const onTap = jest.fn();
    const { getByTestId } = render(
      <TapPad count={0} minimum={8} onTap={onTap} disabled />,
    );
    fireEvent.press(getByTestId("tap-pad"));
    expect(onTap).not.toHaveBeenCalled();
  });
});
