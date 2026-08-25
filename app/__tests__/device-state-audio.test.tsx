/**
 * Tests for the Audio Tuning screen.
 *
 * The screen replaces a generic renderer that showed 14 unlabelled number boxes, so the things
 * worth pinning are the ones that made it usable: Simple mode shows four controls rather than
 * fourteen, every control writes the right characteristic with the right bytes, a device on a
 * value that is not on any macro step says "Custom" instead of lying, and the whole thing
 * degrades rather than crashes on firmware without the service.
 */

import { act, fireEvent, render, waitFor } from "@testing-library/react-native";
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
import { savePresets } from "@/services/audio-preset-store";
import { BUILT_IN_PRESETS } from "@/services/audio-presets";

jest.mock("@react-navigation/bottom-tabs", () => ({ useBottomTabBarHeight: () => 0 }));

// The screen persists presets; keep that off the real filesystem.
jest.mock("@/services/audio-preset-store", () => ({
    loadPresets: jest.fn(() => []),
    savePresets: jest.fn(() => true),
    AUDIO_PRESET_STORE_VERSION: 1,
}));

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

        it("stops the adapt-speed preset at the first failed write", async () => {
            /* Review #413. writeParam resolves false rather than throwing, so discarding the
             * results let a mid-sequence failure carry on and leave a preset no preset defines
             * — attack from the new one, release and rate limit from the old — which
             * adaptSpeedFromFrames then reports as a bland "Custom". */
            const write = jest
                .fn()
                .mockResolvedValueOnce(true) // attack lands
                .mockResolvedValue(false); // release fails
            mockBluetooth(buildDevice(), write);
            const { getByText } = render(<AudioTuningScreen />);

            fireEvent.press(getByText("Fast"));

            await waitFor(() => expect(write).toHaveBeenCalledTimes(2));
            // Third write never issued: one parameter out of step beats three.
            expect(write).toHaveBeenCalledTimes(2);
        });

        it("surfaces a failed write instead of silently snapping back", async () => {
            /* This screen surfaced write errors NOWHERE — alone among the device-state screens.
             * A failed write showed the thumb for the settle window and then reverted, which is
             * indistinguishable from the firmware clamping the value. */
            const device = buildDevice();
            device.characteristicsByService[UUID_AUDIO_CONFIG_SERVICE][
                AUDIO_PARAMS.beatAlpha.uuid
            ].lastWriteError = "GATT write not permitted";
            mockBluetooth(device);
            const { getByTestId } = render(<AudioTuningScreen />);

            expect(getByTestId("audio-write-error").props.children.join("")).toContain(
                "GATT write not permitted",
            );
        });

        it("keeps the Sensitivity slider LIVE on an off-grid board", async () => {
            /* Review #413. `value === null` meant two different things — "not read yet" and "off
             * the 1..10 macro grid" — and disabling on both killed Simple mode on any board
             * tuned over the shell. The caption in this exact state reads "Custom (…) - move the
             * slider to take control", so the screen was instructing a gesture it had disabled.
             *
             * 0.77 is deliberately off every macro step (see audio-params.test.ts). */
            const write = mockBluetooth(buildDevice({ beatAlpha: 0.77 }));
            const { getByTestId, getByText } = render(<AudioTuningScreen />);

            const slider = getByTestId("param-slider-beatAlpha");
            expect(slider.props.disabled).toBeFalsy();
            // The caption that makes this a contradiction rather than a nicety.
            expect(getByText(/move the slider to take control/)).toBeTruthy();

            // And the drag the caption asks for actually reaches the device.
            fireEvent(slider, "slidingComplete", 1);
            await waitFor(() => expect(write).toHaveBeenCalled());
        });

        it("still disables a slider whose characteristic has not been read", () => {
            // The other half of the distinction: an unread value has nowhere to put the thumb,
            // so it must stay disabled rather than inviting a drag from a meaningless position.
            const device = buildDevice();
            device.characteristicsByService[UUID_AUDIO_CONFIG_SERVICE][
                AUDIO_PARAMS.beatAlpha.uuid
            ].value = null;
            mockBluetooth(device);
            const { getByTestId } = render(<AudioTuningScreen />);

            expect(getByTestId("param-slider-beatAlpha").props.disabled).toBe(true);
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

    describe("presets, A/B and undo", () => {
        it("shows the footer controls, with A/B and Undo disabled until they mean something", () => {
            mockBluetooth(buildDevice());
            const { getByTestId } = render(<AudioTuningScreen />);

            // Swapping needs both slots assigned; undo needs something to undo. Offering either
            // as a live button would be a lie about what a tap will do.
            expect(getByTestId("audio-swap-ab").props.accessibilityState.disabled).toBe(true);
            expect(getByTestId("audio-undo").props.accessibilityState.disabled).toBe(true);
            expect(getByTestId("audio-open-presets")).toBeTruthy();
        });

        it("opens the preset sheet listing the built-ins", () => {
            mockBluetooth(buildDevice());
            const { getByTestId, getByText, queryByTestId } = render(<AudioTuningScreen />);

            expect(queryByTestId("preset-sheet")).toBeNull();
            fireEvent.press(getByTestId("audio-open-presets"));

            expect(getByTestId("preset-sheet")).toBeTruthy();
            expect(getByText("Factory defaults")).toBeTruthy();
            expect(getByText("Loud club")).toBeTruthy();
            expect(getByText("Acoustic / quiet set")).toBeTruthy();
        });

        it("says how many settings a preset would change, and disables a no-op apply", () => {
            // The device is on factory defaults, so applying Factory defaults changes nothing.
            mockBluetooth(buildDevice());
            const { getByTestId, getByText } = render(<AudioTuningScreen />);
            fireEvent.press(getByTestId("audio-open-presets"));

            expect(getByText("Already applied")).toBeTruthy();
            expect(getByTestId("preset-apply-builtin:factory").props.accessibilityState.disabled).toBe(true);
            expect(getByTestId("preset-apply-builtin:loud-club").props.accessibilityState.disabled).toBe(false);
        });

        it("applies only the parameters a preset actually changes", async () => {
            const write = mockBluetooth(buildDevice());
            const { getByTestId } = render(<AudioTuningScreen />);
            fireEvent.press(getByTestId("audio-open-presets"));

            await act(async () => {
                fireEvent.press(getByTestId("preset-apply-builtin:loud-club"));
            });

            const loudClub = BUILT_IN_PRESETS.find(p => p.id === "builtin:loud-club")!;
            const expectedKeys = Object.keys(loudClub.values) as (keyof typeof AUDIO_PARAMS)[];
            await waitFor(() => expect(write).toHaveBeenCalledTimes(expectedKeys.length));

            // Every write must be one this preset actually expresses an opinion about.
            const writtenUuids = write.mock.calls.map(c => c[1]);
            const allowed = expectedKeys.map(k => AUDIO_PARAMS[k].uuid);
            writtenUuids.forEach(u => expect(allowed).toContain(u));
        });

        it("enables Undo after an apply and restores the previous values", async () => {
            const write = mockBluetooth(buildDevice());
            const { getByTestId } = render(<AudioTuningScreen />);
            fireEvent.press(getByTestId("audio-open-presets"));

            await act(async () => {
                fireEvent.press(getByTestId("preset-apply-builtin:loud-club"));
            });
            await waitFor(() =>
                expect(getByTestId("audio-undo").props.accessibilityState.disabled).toBe(false),
            );

            write.mockClear();
            await act(async () => {
                fireEvent.press(getByTestId("audio-undo"));
            });

            const loudClub = BUILT_IN_PRESETS.find(p => p.id === "builtin:loud-club")!;
            const keys = Object.keys(loudClub.values) as (keyof typeof AUDIO_PARAMS)[];
            await waitFor(() => expect(write).toHaveBeenCalledTimes(keys.length));

            // Restores the firmware defaults the device started on.
            keys.forEach(key => {
                const spec = AUDIO_PARAMS[key];
                expect(write).toHaveBeenCalledWith(
                    UUID_AUDIO_CONFIG_SERVICE,
                    spec.uuid,
                    encodeParam(spec, spec.defaultValue),
                );
            });
        });

        it("enables A/B only once both slots are assigned", async () => {
            mockBluetooth(buildDevice());
            const { getByTestId } = render(<AudioTuningScreen />);
            fireEvent.press(getByTestId("audio-open-presets"));

            fireEvent.press(getByTestId("preset-slot-a-builtin:factory"));
            await waitFor(() =>
                expect(getByTestId("audio-swap-ab").props.accessibilityState.disabled).toBe(true),
            );

            fireEvent.press(getByTestId("preset-slot-b-builtin:loud-club"));
            await waitFor(() =>
                expect(getByTestId("audio-swap-ab").props.accessibilityState.disabled).toBe(false),
            );
        });

        it("saves from the keyboard's done key, not just the Save button", async () => {
            // On a real phone the on-screen keyboard covers the Save button outright — the name
            // field sits at the bottom of a bottom-anchored sheet, which is exactly where the
            // keyboard lands (found on a Pixel 9 Pro). The sheet now avoids the keyboard, and
            // submitting from the keyboard saves directly, which is the natural gesture anyway.
            mockBluetooth(buildDevice());
            const { getByTestId, getByText } = render(<AudioTuningScreen />);
            fireEvent.press(getByTestId("audio-open-presets"));

            fireEvent.changeText(getByTestId("preset-save-name"), "From Keyboard");
            await act(async () => {
                fireEvent(getByTestId("preset-save-name"), "submitEditing");
            });

            await waitFor(() => expect(getByText('Saved "From Keyboard"')).toBeTruthy());
            expect(savePresets).toHaveBeenCalled();
        });

        it("saves the current values under a name", async () => {
            mockBluetooth(buildDevice());
            const { getByTestId, getByText } = render(<AudioTuningScreen />);
            fireEvent.press(getByTestId("audio-open-presets"));

            fireEvent.changeText(getByTestId("preset-save-name"), "Warehouse");
            await act(async () => {
                fireEvent.press(getByTestId("preset-save"));
            });

            await waitFor(() => expect(getByText('Saved "Warehouse"')).toBeTruthy());
            expect(savePresets).toHaveBeenCalled();
        });
    });
});
