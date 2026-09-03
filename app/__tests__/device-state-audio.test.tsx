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
import {
    UUID_AUDIO_CONFIG_SERVICE,
    UUID_AUDIO_PARAM_RANGES,
    UUID_AUDIO_TELEMETRY_SERVICE,
} from "@/constants/bluetooth";
import * as BluetoothContext from "@/context/bluetooth-context";
import { parseAudioParamRanges } from "@/services/audio-param-ranges";
import {
    AUDIO_PARAMS,
    AUDIO_PARAM_ORDER,
    SENSITIVITY_MAX,
    alphaFromSensitivity,
    encodeParam,
    gateFromNoiseLevel,
    type AudioParamKey,
    type AudioParamSpec,
} from "@/services/audio-params";
import { savePresets } from "@/services/audio-preset-store";
import * as presetStore from "@/services/audio-preset-store";
import { BUILT_IN_PRESETS } from "@/services/audio-presets";

jest.mock("@react-navigation/bottom-tabs", () => ({ useBottomTabBarHeight: () => 0 }));

// The screen persists presets; keep that off the real filesystem.
/* A store that actually stores. `loadPresets: () => []` reads back as an empty disk no matter
 * what was written, which lets a test pass on in-memory state alone — and the hook is allowed to
 * treat the file as the source of truth (another screen can add a preset while this one is
 * mounted), so that difference is load-bearing rather than cosmetic. */
jest.mock("@/services/audio-preset-store", () => {
    const state: { presets: unknown[] } = { presets: [] };
    return {
        __state: state,
        loadPresets: jest.fn(() => state.presets),
        savePresets: jest.fn((next: unknown[]) => {
            state.presets = next;
            return true;
        }),
        AUDIO_PRESET_STORE_VERSION: 1,
    };
});

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

/**
 * Hand-built ranges blob (the wire format in services/audio-param-ranges.ts), mirroring the
 * app table except where overridden — so a test can model a device whose firmware publishes a
 * range the app's mirror does not know about.
 */
function makeRangesBlob(
    rangeOverrides: Partial<Record<AudioParamKey, Partial<Pick<AudioParamSpec, "min" | "max">>>> = {},
): string {
    const bytes: number[] = [1, AUDIO_PARAM_ORDER.length];
    const pushF32 = (v: number) => {
        const buf = new ArrayBuffer(4);
        new DataView(buf).setFloat32(0, v, true);
        bytes.push(...new Uint8Array(buf));
    };
    AUDIO_PARAM_ORDER.forEach(key => {
        const spec = AUDIO_PARAMS[key];
        const o = rangeOverrides[key] ?? {};
        bytes.push(spec.kind === "enum" ? 2 : spec.kind === "uint" ? 1 : 0);
        bytes.push(0); // unit_len
        bytes.push(0); // enum_len
        pushF32(spec.defaultValue);
        pushF32(o.min ?? spec.min);
        pushF32(o.max ?? spec.max);
        pushF32(0); // step 0 = keep the app's
    });
    let s = "";
    for (const b of bytes) s += String.fromCharCode(b);
    return btoa(s);
}

/** buildDevice plus a telemetry service carrying the given ranges blob. */
function buildDeviceWithRanges(blob: string) {
    const device = buildDevice();
    device.services.push({ uuid: UUID_AUDIO_TELEMETRY_SERVICE });
    (device.characteristicsByService as Record<string, any>)[UUID_AUDIO_TELEMETRY_SERVICE] = {
        [UUID_AUDIO_PARAM_RANGES]: {
            characteristic: {},
            value: blob,
            name: "Audio Param Ranges",
            cpfFormat: 0,
            isUpdateInProgress: false,
        },
    };
    return device;
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

/** The mocked store keeps state across tests in a file; each test starts from an empty disk. */
function resetPresetStore() {
    (presetStore as unknown as { __state: { presets: unknown[] } }).__state.presets = [];
}

describe("AudioTuningScreen", () => {
    beforeEach(() => {
        jest.spyOn(console, "log").mockImplementation(() => {});
        resetPresetStore();
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
                encodeParam(AUDIO_PARAMS.beatAlpha, alphaFromSensitivity(SENSITIVITY_MAX)),
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
             * the macro grid" — and disabling on both killed Simple mode on any board
             * tuned over the shell. The caption in this exact state reads "Custom (…) - move the
             * slider to take control", so the screen was instructing a gesture it had disabled.
             *
             * 0.9 is deliberately off every macro step (see audio-params.test.ts; 0.77 held this
             * role until the 1..20 grid put step 8 within inversion tolerance of it). */
            const write = mockBluetooth(buildDevice({ beatAlpha: 0.9 }));
            const { getByTestId, getByText } = render(<AudioTuningScreen />);

            const slider = getByTestId("param-slider-beatAlpha");
            expect(slider.props.disabled).toBeFalsy();
            // The caption that makes this a contradiction rather than a nicety.
            expect(getByText(/move the slider to take control/)).toBeTruthy();

            // And the drag the caption asks for actually reaches the device.
            fireEvent(slider, "slidingComplete", 1);
            await waitFor(() => expect(write).toHaveBeenCalled());
        });

        it("computes the low steps from a device-published max, so they stay distinct (review #425)", async () => {
            /* A device publishing beatAlpha max = 10 (narrower than the mirror's 20). The
             * curve endpoints derive from the spec, so a mirror-built curve would compute 20.0
             * and 12.6 for steps 1 and 2, encode-clamp BOTH to 10.0, and the read-back would
             * flip the slider to Custom right after the user picked a step. With the resolved
             * spec threaded through, step 2 lands strictly inside the device's own range. */
            const blob = makeRangesBlob({ beatAlpha: { max: 10 } });
            const write = mockBluetooth(buildDeviceWithRanges(blob));
            const { getByTestId } = render(<AudioTuningScreen />);

            // Position 1/19 of the 1..20 travel = step 2, the first step past the bottom.
            fireEvent(getByTestId("param-slider-beatAlpha"), "slidingComplete", 1 / 19);

            await waitFor(() => expect(write).toHaveBeenCalled());
            const resolved = {
                ...AUDIO_PARAMS.beatAlpha,
                ...parseAudioParamRanges(blob)!.beatAlpha,
            } as AudioParamSpec;
            const expected = alphaFromSensitivity(2, resolved);
            expect(expected).toBeLessThan(10); // distinct from step 1 = the device's max
            expect(write).toHaveBeenCalledWith(
                UUID_AUDIO_CONFIG_SERVICE,
                AUDIO_PARAMS.beatAlpha.uuid,
                encodeParam(resolved, expected),
            );
        });

        it("keeps the noise macro inside a device-published gate range (review #425)", async () => {
            /* The pre-existing counterexample from the PR thread: the noise handlers encoded
             * against the static mirror. On a device publishing gate max = 0.002, the macro's
             * top step (mirror value 0.004) must clamp to the device's own range. */
            const blob = makeRangesBlob({ agcNoiseGateRms: { max: 0.002 } });
            const write = mockBluetooth(buildDeviceWithRanges(blob));
            const { getByTestId } = render(<AudioTuningScreen />);

            fireEvent(getByTestId("param-slider-agcNoiseGateRms"), "slidingComplete", 1);

            await waitFor(() => expect(write).toHaveBeenCalled());
            const resolved = {
                ...AUDIO_PARAMS.agcNoiseGateRms,
                ...parseAudioParamRanges(blob)!.agcNoiseGateRms,
            } as AudioParamSpec;
            const expected = gateFromNoiseLevel(10, resolved);
            expect(expected).toBeCloseTo(0.002, 9);
            expect(write).toHaveBeenCalledWith(
                UUID_AUDIO_CONFIG_SERVICE,
                AUDIO_PARAMS.agcNoiseGateRms.uuid,
                encodeParam(resolved, expected),
            );
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
                getByText(
                    "These only change the bar visualiser (a dB meter: floor, span, treble lift, gain). " +
                        "They do not affect beat detection.",
                ),
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

        it("re-suggests a fresh name each time the sheet opens", async () => {
            /* A Modal keeps its children mounted while hidden, so the name field's useState
             * initialiser ran once per app launch. The field then held the first suggestion
             * forever, and because saving overwrites by name, the SECOND save of a session
             * silently replaced the first preset while announcing a plain "Saved". */
            mockBluetooth(buildDevice());
            const { getByTestId } = render(<AudioTuningScreen />);

            fireEvent.press(getByTestId("audio-open-presets"));
            fireEvent.changeText(getByTestId("preset-save-name"), "Typed over");
            expect(getByTestId("preset-save-name").props.value).toBe("Typed over");

            await act(async () => {
                fireEvent.press(getByTestId("preset-save"));
            });

            await act(async () => {
                fireEvent.press(getByTestId("audio-open-presets"));
            });

            expect(getByTestId("preset-save-name").props.value).toMatch(/^Tuned \d{2}:\d{2}$/);
        });

        it("says the preset will not survive a relaunch when the disk write failed", async () => {
            (savePresets as jest.Mock).mockReturnValueOnce(false);
            mockBluetooth(buildDevice());
            const { getByTestId, getByText } = render(<AudioTuningScreen />);
            fireEvent.press(getByTestId("audio-open-presets"));

            fireEvent.changeText(getByTestId("preset-save-name"), "Warehouse");
            await act(async () => {
                fireEvent.press(getByTestId("preset-save"));
            });

            // Not `Saved "Warehouse"` - it is not saved anywhere that outlives the process.
            await waitFor(() =>
                expect(
                    getByText('Could not save "Warehouse" to storage - it will be gone next launch'),
                ).toBeTruthy(),
            );
        });

        it("says Replaced, not Saved, when a save overwrites a same-named preset", async () => {
            mockBluetooth(buildDevice());
            const { getByTestId, getByText } = render(<AudioTuningScreen />);

            fireEvent.press(getByTestId("audio-open-presets"));
            fireEvent.changeText(getByTestId("preset-save-name"), "Warehouse");
            await act(async () => {
                fireEvent.press(getByTestId("preset-save"));
            });
            await waitFor(() => expect(getByText('Saved "Warehouse"')).toBeTruthy());

            fireEvent.press(getByTestId("audio-open-presets"));
            fireEvent.changeText(getByTestId("preset-save-name"), "Warehouse");
            await act(async () => {
                fireEvent.press(getByTestId("preset-save"));
            });
            await waitFor(() => expect(getByText('Replaced "Warehouse"')).toBeTruthy());
        });

        it("does not report a restore as successful when every write failed", async () => {
            const write = jest.fn().mockResolvedValue(true);
            mockBluetooth(buildDevice(), write);
            const { getByTestId, getByText } = render(<AudioTuningScreen />);

            fireEvent.press(getByTestId("audio-open-presets"));
            await act(async () => {
                fireEvent.press(getByTestId("preset-apply-builtin:loud-club"));
            });
            await waitFor(() =>
                expect(getByTestId("audio-undo").props.accessibilityState.disabled).toBe(false),
            );

            write.mockResolvedValue(false);
            await act(async () => {
                fireEvent.press(getByTestId("audio-undo"));
            });

            // `applied` alone read every outcome as success: a restore in which nothing landed
            // announced "(0 restored)" in the same phrasing as one that worked.
            await waitFor(() => expect(getByText(/^Could not undo /)).toBeTruthy());
        });

        it("announces which preset an A/B tap moved to, not a bare Swapped", async () => {
            mockBluetooth(buildDevice());
            const { getByTestId, getByText } = render(<AudioTuningScreen />);

            fireEvent.press(getByTestId("audio-open-presets"));
            fireEvent.press(getByTestId("preset-slot-a-builtin:factory"));
            fireEvent.press(getByTestId("preset-slot-b-builtin:loud-club"));
            fireEvent.press(getByTestId("preset-sheet"));

            await act(async () => {
                fireEvent.press(getByTestId("audio-swap-ab"));
            });

            await waitFor(() => expect(getByText(/^Now on "Loud club"/)).toBeTruthy());
        });

        it("gives the disabled Apply control an accessibilityState a screen reader can read", () => {
            /* The hand-rolled Apply button dimmed itself with opacity and set `disabled`, but
             * omitted accessibilityState - so a screen reader announced an Apply that does
             * nothing as tappable. AppButton has always supplied it. */
            mockBluetooth(buildDevice());
            const { getByTestId } = render(<AudioTuningScreen />);
            fireEvent.press(getByTestId("audio-open-presets"));

            // The device is at factory defaults, so "Factory defaults" changes nothing.
            expect(
                getByTestId("preset-apply-builtin:factory").props.accessibilityState.disabled,
            ).toBe(true);
            expect(
                getByTestId("preset-apply-builtin:loud-club").props.accessibilityState.disabled,
            ).toBe(false);
        });
    });
});
