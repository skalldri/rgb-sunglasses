/**
 * Tests for the Audio Tuning screen.
 *
 * The screen replaces a generic renderer that showed 14 unlabelled number boxes, so the things
 * worth pinning are the ones that made it usable: Simple mode shows four controls rather than
 * fourteen, every control writes the right characteristic with the right bytes, a device on a
 * value that is not on any macro step says "Custom" instead of lying, and the whole thing
 * degrades rather than crashes on firmware without the service.
 */

import { fireEvent, render, waitFor } from "@testing-library/react-native";
import React from "react";

import AudioTuningScreen from "@/app/(tabs)/device-state/audio";
import { UUID_AUDIO_CONFIG_SERVICE } from "@/constants/bluetooth";
import * as BluetoothContext from "@/context/bluetooth-context";
import {
    AUDIO_PARAMS,
    AUDIO_PARAM_ORDER,
    alphaFromSensitivity,
    encodeParam,
    gateFromNoiseLevel,
} from "@/services/audio-params";

jest.mock("@react-navigation/bottom-tabs", () => ({ useBottomTabBarHeight: () => 0 }));

/** Build a device exposing the audio service with the firmware defaults, unless overridden. */
function buildDevice(overrides: Partial<Record<keyof typeof AUDIO_PARAMS, number>> = {}) {
    const byService: Record<string, any> = { [UUID_AUDIO_CONFIG_SERVICE]: {} };
    const flat: Record<string, any> = {};

    AUDIO_PARAM_ORDER.forEach(key => {
        const spec = AUDIO_PARAMS[key];
        const value = overrides[key] ?? spec.defaultValue;
        const info = {
            characteristic: {},
            value: encodeParam(spec, value),
            name: spec.firmwareLabel,
            cpfFormat: spec.cpfFormat,
            isUpdateInProgress: false,
        };
        byService[UUID_AUDIO_CONFIG_SERVICE][spec.uuid] = info;
        flat[spec.uuid] = info;
    });

    return {
        name: "RGB Sunglasses",
        mac: "AA:BB:CC",
        device: {},
        services: [{ uuid: UUID_AUDIO_CONFIG_SERVICE }],
        characteristicsByService: byService,
        characteristics: flat,
        serviceCharacteristics: {
            [UUID_AUDIO_CONFIG_SERVICE]: AUDIO_PARAM_ORDER.map(k => AUDIO_PARAMS[k].uuid),
        },
    };
}

function mockBluetooth(device: any, writeServiceCharacteristic = jest.fn().mockResolvedValue(true)) {
    jest.spyOn(BluetoothContext, "useBluetooth").mockImplementation(
        () =>
            ({
                selectedDevice: device,
                writeServiceCharacteristic,
                writeToCharacteristic: jest.fn().mockResolvedValue(true),
                updateCharValue: jest.fn(),
                getCharacteristicInfo: jest.fn(),
            }) as any,
    );
    return writeServiceCharacteristic;
}

describe("AudioTuningScreen", () => {
    beforeEach(() => {
        jest.spyOn(console, "log").mockImplementation(() => {});
    });
    afterEach(() => jest.restoreAllMocks());

    describe("degradation", () => {
        it("tells the user to connect when there is no device", () => {
            mockBluetooth(null);
            const { getByText } = render(<AudioTuningScreen />);
            expect(getByText("Not connected")).toBeTruthy();
        });

        it("degrades gracefully on firmware without the audio service", () => {
            mockBluetooth({
                name: "RGB Sunglasses",
                mac: "AA:BB:CC",
                services: [],
                characteristicsByService: {},
                characteristics: {},
                serviceCharacteristics: {},
            });
            const { getByText } = render(<AudioTuningScreen />);
            expect(getByText("No audio tuning on this firmware")).toBeTruthy();
        });
    });

    describe("simple mode", () => {
        it("shows four plain-language controls, not fourteen parameters", () => {
            mockBluetooth(buildDevice());
            const { getByText, queryByText } = render(<AudioTuningScreen />);

            expect(getByText("Sensitivity")).toBeTruthy();
            expect(getByText("Beat feel")).toBeTruthy();
            expect(getByText("Ignore background noise")).toBeTruthy();
            expect(getByText("How fast it adapts")).toBeTruthy();

            // Raw firmware parameters must stay out of Simple mode entirely.
            expect(queryByText("Flux Gamma")).toBeNull();
            expect(queryByText("Bass/treble balance")).toBeNull();
            expect(queryByText("FFT Energy Scale")).toBeNull();
        });

        it("writes Beat Alpha when Sensitivity moves", async () => {
            const write = mockBluetooth(buildDevice());
            const { getByTestId } = render(<AudioTuningScreen />);

            fireEvent(getByTestId("param-slider-beatAlpha"), "slidingComplete", 1);

            await waitFor(() => expect(write).toHaveBeenCalled());
            expect(write).toHaveBeenCalledWith(
                UUID_AUDIO_CONFIG_SERVICE,
                AUDIO_PARAMS.beatAlpha.uuid,
                encodeParam(AUDIO_PARAMS.beatAlpha, alphaFromSensitivity(10)),
            );
        });

        it("writes the noise gate when Ignore background noise moves", async () => {
            const write = mockBluetooth(buildDevice());
            const { getByTestId } = render(<AudioTuningScreen />);

            // Slider position 0 is the reserved "Off" end of the travel.
            fireEvent(getByTestId("param-slider-agcNoiseGateRms"), "slidingComplete", 0);

            await waitFor(() => expect(write).toHaveBeenCalled());
            expect(write).toHaveBeenCalledWith(
                UUID_AUDIO_CONFIG_SERVICE,
                AUDIO_PARAMS.agcNoiseGateRms.uuid,
                encodeParam(AUDIO_PARAMS.agcNoiseGateRms, gateFromNoiseLevel("off")),
            );
        });

        it("writes the refractory frames a Beat feel pill stands for", async () => {
            const write = mockBluetooth(buildDevice());
            const { getByTestId } = render(<AudioTuningScreen />);

            fireEvent.press(getByTestId("choice-beat-feel-Kick only"));

            await waitFor(() => expect(write).toHaveBeenCalled());
            expect(write).toHaveBeenCalledWith(
                UUID_AUDIO_CONFIG_SERVICE,
                AUDIO_PARAMS.beatRefractoryFrames.uuid,
                encodeParam(AUDIO_PARAMS.beatRefractoryFrames, 12),
            );
        });

        it("writes all three AGC timings sequentially for an adapt-speed pill", async () => {
            const write = mockBluetooth(buildDevice());
            const { getByTestId } = render(<AudioTuningScreen />);

            fireEvent.press(getByTestId("choice-adapt-speed-Fast"));

            // Android permits one outstanding GATT op, so these must be three separate awaited
            // writes rather than a concurrent burst.
            await waitFor(() => expect(write).toHaveBeenCalledTimes(3));
            expect(write).toHaveBeenCalledWith(
                UUID_AUDIO_CONFIG_SERVICE,
                AUDIO_PARAMS.agcAttackFrames.uuid,
                encodeParam(AUDIO_PARAMS.agcAttackFrames, 2),
            );
            expect(write).toHaveBeenCalledWith(
                UUID_AUDIO_CONFIG_SERVICE,
                AUDIO_PARAMS.agcReleaseFrames.uuid,
                encodeParam(AUDIO_PARAMS.agcReleaseFrames, 6),
            );
            expect(write).toHaveBeenCalledWith(
                UUID_AUDIO_CONFIG_SERVICE,
                AUDIO_PARAMS.agcRateLimitFrames.uuid,
                encodeParam(AUDIO_PARAMS.agcRateLimitFrames, 4),
            );
        });

        it("highlights the pill matching the device's stored values", () => {
            mockBluetooth(buildDevice());
            const { getByTestId } = render(<AudioTuningScreen />);
            // Firmware defaults are refractory 5 / attack 3 / release 15 / rate 10 = "Normal".
            expect(getByTestId("choice-beat-feel-Normal").props.accessibilityState.selected).toBe(true);
            expect(getByTestId("choice-adapt-speed-Normal").props.accessibilityState.selected).toBe(true);
        });

        it("says Custom rather than highlighting the nearest pill", () => {
            // 7 frames is not any Beat feel preset.
            mockBluetooth(buildDevice({ beatRefractoryFrames: 7 }));
            const { getByText, getByTestId } = render(<AudioTuningScreen />);

            expect(getByText("Custom (224 ms)")).toBeTruthy();
            expect(getByTestId("choice-beat-feel-Normal").props.accessibilityState.selected).toBe(false);
        });

        it("follows the device into median mode and drives SF Delta instead of Alpha", async () => {
            const write = mockBluetooth(buildDevice({ beatThresholdMode: 1 }));
            const { getByTestId, queryByTestId } = render(<AudioTuningScreen />);

            // The control's identity must follow the mode — Simple mode deliberately hides
            // firmware names, so this is checked via the control id rather than visible text.
            expect(queryByTestId("param-slider-beatAlpha")).toBeNull();

            fireEvent(getByTestId("param-slider-beatSfDelta"), "slidingComplete", 0);
            await waitFor(() => expect(write).toHaveBeenCalled());
            expect(write.mock.calls[0][1]).toBe(AUDIO_PARAMS.beatSfDelta.uuid);
        });
    });

    describe("advanced mode", () => {
        it("reveals the raw firmware parameters with both names", () => {
            mockBluetooth(buildDevice());
            const { getByText } = render(<AudioTuningScreen />);

            fireEvent.press(getByText("Advanced"));

            expect(getByText("Bass/treble balance")).toBeTruthy();
            expect(getByText("Flux Gamma")).toBeTruthy(); // greps against `sound dsp params`
            expect(getByText("Automatic gain")).toBeTruthy();
            expect(getByText("Beat detection")).toBeTruthy();
        });

        it("warns that the bar-display parameters do not affect beat detection", () => {
            mockBluetooth(buildDevice());
            const { getByText } = render(<AudioTuningScreen />);
            fireEvent.press(getByText("Advanced"));

            expect(
                getByText("These only change the bar visualiser. They do not affect beat detection."),
            ).toBeTruthy();
        });

        it("hides the threshold parameter the device is not currently using", () => {
            mockBluetooth(buildDevice({ beatThresholdMode: 0 }));
            const { getByText, queryByText } = render(<AudioTuningScreen />);
            fireEvent.press(getByText("Advanced"));

            // Mode 0 uses alpha; showing a live SF Delta slider would invite tuning a value the
            // detector ignores.
            expect(getByText("Sensitivity")).toBeTruthy();
            expect(queryByText("Sensitivity (median mode)")).toBeNull();
        });

        it("shows frame counts in milliseconds, not frames", () => {
            mockBluetooth(buildDevice());
            const { getByText, getByTestId } = render(<AudioTuningScreen />);
            fireEvent.press(getByText("Advanced"));

            expect(getByTestId("param-value-agcReleaseFrames").props.children).toBe("480 ms");
        });

        it("writes the encoded value for a raw slider", async () => {
            const write = mockBluetooth(buildDevice());
            const { getByText, getByTestId } = render(<AudioTuningScreen />);
            fireEvent.press(getByText("Advanced"));

            fireEvent(getByTestId("param-slider-fluxGamma"), "slidingComplete", 1);

            await waitFor(() => expect(write).toHaveBeenCalled());
            expect(write).toHaveBeenCalledWith(
                UUID_AUDIO_CONFIG_SERVICE,
                AUDIO_PARAMS.fluxGamma.uuid,
                encodeParam(AUDIO_PARAMS.fluxGamma, AUDIO_PARAMS.fluxGamma.max),
            );
        });
    });

    describe("help", () => {
        it("opens per-parameter help without leaving the screen", () => {
            mockBluetooth(buildDevice());
            const { getByTestId, getByText, queryByTestId } = render(<AudioTuningScreen />);

            expect(queryByTestId("audio-help-sheet")).toBeNull();
            fireEvent.press(getByTestId("param-help-beatAlpha"));

            // A modal, not a route: the meters (Phase 3) must stay mounted behind it.
            expect(getByTestId("audio-help-sheet")).toBeTruthy();
            expect(getByText("Firmware name: Beat Alpha")).toBeTruthy();
        });
    });

    describe("read behaviour", () => {
        it("does not read characteristics on render", () => {
            // The audio characteristics are not notifiable, which makes a read-on-focus effect
            // tempting — and it is exactly the unbounded-read-loop shape documented in
            // app/CLAUDE.md (measured at 110 reads in 10 s). Values come from discovery instead.
            const device = buildDevice();
            const read = jest.fn().mockResolvedValue({ value: null });
            Object.values(device.characteristicsByService[UUID_AUDIO_CONFIG_SERVICE]).forEach(
                (info: any) => {
                    info.characteristic.read = read;
                },
            );
            mockBluetooth(device);

            const { rerender } = render(<AudioTuningScreen />);
            rerender(<AudioTuningScreen />);
            rerender(<AudioTuningScreen />);

            expect(read).not.toHaveBeenCalled();
        });
    });
});
