/**
 * Tests for the Audio Tuning write coordinator.
 *
 * The throttle and the settle window are the parts most likely to be "fixed" later by someone
 * who does not know why they are there, so each test states the consequence of getting it wrong.
 */

import { act, renderHook } from "@testing-library/react-native";

import {
    SETTLE_MS,
    WRITE_THROTTLE_MS,
    useAudioParamWriter,
} from "@/hooks/use-audio-param-writer";

const UUID = "12345678-1234-5678-0002-56789abc0002";
const encode = (v: number) => `enc:${v}`;

describe("useAudioParamWriter", () => {
    beforeEach(() => jest.useFakeTimers());
    afterEach(() => {
        // Draining the settle timer resolves a setState, so it has to happen inside act() or
        // every test logs a spurious "not wrapped in act(...)" warning.
        act(() => {
            jest.runOnlyPendingTimers();
        });
        jest.useRealTimers();
    });

    it("writes the leading edge of a drag immediately", async () => {
        const write = jest.fn().mockResolvedValue(true);
        const { result } = renderHook(() => useAudioParamWriter({ write }));

        await act(async () => {
            result.current.onSlide(UUID, 1, encode);
        });

        expect(write).toHaveBeenCalledTimes(1);
        expect(write).toHaveBeenCalledWith(UUID, "enc:1");
    });

    it("throttles a rapid drag instead of flooding the GATT queue", async () => {
        const write = jest.fn().mockResolvedValue(true);
        const { result } = renderHook(() => useAudioParamWriter({ write }));

        // 20 slider events inside one throttle window.
        await act(async () => {
            for (let i = 0; i < 20; i++) result.current.onSlide(UUID, i, encode);
        });
        expect(write).toHaveBeenCalledTimes(1);

        // The trailing edge then writes where the thumb actually ended up — writing the value
        // from the START of the window would leave the device on a value the user never chose.
        await act(async () => {
            jest.advanceTimersByTime(WRITE_THROTTLE_MS);
        });
        expect(write).toHaveBeenCalledTimes(2);
        expect(write).toHaveBeenLastCalledWith(UUID, "enc:19");
    });

    it("does not rewrite an unchanged encoded value", async () => {
        const write = jest.fn().mockResolvedValue(true);
        const { result } = renderHook(() => useAudioParamWriter({ write }));

        await act(async () => {
            result.current.onSlide(UUID, 5, encode);
        });
        await act(async () => {
            jest.advanceTimersByTime(WRITE_THROTTLE_MS * 2);
            result.current.onSlide(UUID, 5, encode);
        });

        // Many slider positions snap to the same value; each must not cost a round-trip.
        expect(write).toHaveBeenCalledTimes(1);
    });

    it("writes the final position once on slide complete", async () => {
        const write = jest.fn().mockResolvedValue(true);
        const { result } = renderHook(() => useAudioParamWriter({ write }));

        await act(async () => {
            result.current.onSlide(UUID, 1, encode);
            result.current.onSlide(UUID, 2, encode);
            result.current.onSlideComplete(UUID, 3, encode);
        });

        // The queued intermediate write must be cancelled, not delivered after the final one.
        expect(write).toHaveBeenLastCalledWith(UUID, "enc:3");
        await act(async () => {
            jest.advanceTimersByTime(WRITE_THROTTLE_MS * 2);
        });
        expect(write).toHaveBeenLastCalledWith(UUID, "enc:3");
    });

    describe("thumb ownership", () => {
        it("ignores the context value while the local override is active", async () => {
            const write = jest.fn().mockResolvedValue(true);
            const { result } = renderHook(() => useAudioParamWriter({ write }));

            await act(async () => {
                result.current.onSlideComplete(UUID, 7, encode);
            });

            // This is the clamp read-back arriving at 150 ms. It must NOT move the thumb yet.
            expect(result.current.displayValue(UUID, 999)).toBe(7);
            expect(result.current.isOwned(UUID)).toBe(true);

            await act(async () => {
                jest.advanceTimersByTime(SETTLE_MS - 100);
            });
            expect(result.current.displayValue(UUID, 999)).toBe(7);
        });

        it("hands the thumb back to context once the settle window closes", async () => {
            const write = jest.fn().mockResolvedValue(true);
            const { result } = renderHook(() => useAudioParamWriter({ write }));

            await act(async () => {
                result.current.onSlideComplete(UUID, 7, encode);
            });
            await act(async () => {
                jest.advanceTimersByTime(SETTLE_MS + 50);
            });

            // Now the firmware's clamped value becomes visible — exactly once, and at a moment
            // the user is looking at the control rather than mid-drag.
            expect(result.current.isOwned(UUID)).toBe(false);
            expect(result.current.displayValue(UUID, 999)).toBe(999);
        });

        it("outlasts the 1200 ms clamp read-back in context/bluetooth-context.tsx", () => {
            // If this ever stops holding, the thumb is handed back before the clamped value
            // lands and the snap appears to happen at random.
            expect(SETTLE_MS).toBeGreaterThan(1200);
        });

        it("leaves untouched parameters showing their context value", () => {
            const write = jest.fn().mockResolvedValue(true);
            const { result } = renderHook(() => useAudioParamWriter({ write }));

            expect(result.current.displayValue("other-uuid", 42)).toBe(42);
            expect(result.current.isOwned("other-uuid")).toBe(false);
        });
    });

    it("keeps per-parameter state separate", async () => {
        const write = jest.fn().mockResolvedValue(true);
        const { result } = renderHook(() => useAudioParamWriter({ write }));
        const other = "12345678-1234-5678-0002-56789abc0003";

        await act(async () => {
            result.current.onSlide(UUID, 1, encode);
            result.current.onSlide(other, 2, encode);
        });

        // Two different sliders are two independent throttles, not one shared one.
        expect(write).toHaveBeenCalledTimes(2);
        expect(write).toHaveBeenCalledWith(UUID, "enc:1");
        expect(write).toHaveBeenCalledWith(other, "enc:2");
    });

    it("writes immediately for pills and macro buttons", async () => {
        const write = jest.fn().mockResolvedValue(true);
        const { result } = renderHook(() => useAudioParamWriter({ write }));

        await act(async () => {
            await result.current.writeNow(UUID, 12, encode);
        });

        expect(write).toHaveBeenCalledWith(UUID, "enc:12");
        expect(result.current.isOwned(UUID)).toBe(true);
    });

    describe("failure handling", () => {
        it("reports a rejected write and allows a retry of the same value", async () => {
            const write = jest.fn().mockRejectedValue(new Error("GATT busy"));
            const onError = jest.fn();
            const { result } = renderHook(() => useAudioParamWriter({ write, onError }));

            await act(async () => {
                await result.current.writeNow(UUID, 3, encode);
            });
            expect(onError).toHaveBeenCalledWith(UUID, expect.any(Error));

            // The dedup cache must not latch a value that never reached the device, or the
            // retry would be silently swallowed.
            await act(async () => {
                await result.current.writeNow(UUID, 3, encode);
            });
            expect(write).toHaveBeenCalledTimes(2);
        });

        it("does not throw when a write rejects during a drag", async () => {
            const write = jest.fn().mockRejectedValue(new Error("GATT busy"));
            const { result } = renderHook(() => useAudioParamWriter({ write }));

            await act(async () => {
                result.current.onSlide(UUID, 1, encode);
            });
            expect(write).toHaveBeenCalledTimes(1);
        });
    });

    it("cancels pending timers on unmount", async () => {
        const write = jest.fn().mockResolvedValue(true);
        const { result, unmount } = renderHook(() => useAudioParamWriter({ write }));

        await act(async () => {
            result.current.onSlide(UUID, 1, encode);
            result.current.onSlide(UUID, 2, encode);
        });
        const callsBeforeUnmount = write.mock.calls.length;

        unmount();
        act(() => {
            jest.advanceTimersByTime(SETTLE_MS * 2);
        });

        // A deferred write firing into a torn-down screen is the failure mode documented in
        // app/CLAUDE.md; read()/write() throw synchronously once the characteristic is gone.
        expect(write).toHaveBeenCalledTimes(callsBeforeUnmount);
    });
});
