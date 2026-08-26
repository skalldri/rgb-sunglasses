/**
 * Tests for preset apply, undo and A/B swap.
 *
 * Two behaviours here are load-bearing at a venue and easy to regress:
 *   - writes are SEQUENTIAL (Android permits one outstanding GATT op, so a concurrent burst
 *     gets most of them rejected);
 *   - only the differing parameters are written, which is what makes an A/B swap fast enough to
 *     hear the difference within a bar.
 */

/* A STATEFUL fake store: savePresets remembers, loadPresets returns what was saved.
 *
 * The old mock returned [] from loadPresets unconditionally, which is not how any real store
 * behaves — and it hid the property that matters now that commits re-read before mutating:
 * two hook instances sharing one file must not clobber each other's entries. A static mock
 * cannot express that at all. */
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

import { act, renderHook, waitFor } from "@testing-library/react-native";

import { UNDO_DEPTH, useAudioPresets } from "@/hooks/use-audio-presets";
import { AUDIO_PARAMS, AUDIO_PARAM_ORDER, AudioParamKey } from "@/services/audio-params";
import { savePresets } from "@/services/audio-preset-store";
import * as presetStore from "@/services/audio-preset-store";
import { BUILT_IN_PRESETS } from "@/services/audio-presets";

const factory = (): Record<AudioParamKey, number> => {
    const out = {} as Record<AudioParamKey, number>;
    AUDIO_PARAM_ORDER.forEach(k => (out[k] = AUDIO_PARAMS[k].defaultValue));
    return out;
};

const loudClub = BUILT_IN_PRESETS.find(p => p.id === "builtin:loud-club")!;
const factoryPreset = BUILT_IN_PRESETS.find(p => p.id === "builtin:factory")!;

function setup(currentValues = factory(), writeParam = jest.fn().mockResolvedValue(true)) {
    const hook = renderHook(() => useAudioPresets({ currentValues, writeParam }));
    return { ...hook, writeParam, currentValues };
}

/** The mocked store keeps state across tests in a file; each test starts from an empty disk. */
function resetPresetStore() {
    (presetStore as unknown as { __state: { presets: unknown[] } }).__state.presets = [];
}

describe("useAudioPresets", () => {
    beforeEach(() => {
        jest.clearAllMocks();
        // Reset the fake store between tests, or saved presets leak across them.
        resetPresetStore();
    });

    it("exposes the built-ins plus anything saved", () => {
        const { result } = setup();
        expect(result.current.allPresets.length).toBe(BUILT_IN_PRESETS.length);
        expect(result.current.builtIns).toBe(BUILT_IN_PRESETS);
    });

    describe("applyPreset", () => {
        it("writes only the parameters that differ", async () => {
            const { result, writeParam } = setup();
            await act(async () => {
                await result.current.applyPreset(loudClub);
            });

            const expected = Object.keys(loudClub.values).length;
            expect(writeParam).toHaveBeenCalledTimes(expected);
            // Nothing outside the preset's own opinion should be touched.
            const written = writeParam.mock.calls.map(c => c[0]);
            written.forEach(k => expect(Object.keys(loudClub.values)).toContain(k));
        });

        it("writes nothing when the device already matches", async () => {
            const { result, writeParam } = setup();
            await act(async () => {
                await result.current.applyPreset(factoryPreset);
            });
            expect(writeParam).not.toHaveBeenCalled();
        });

        it("writes sequentially, never concurrently", async () => {
            // Android permits one outstanding GATT operation; a concurrent burst gets rejected.
            let inFlight = 0;
            let maxInFlight = 0;
            const writeParam = jest.fn(async () => {
                inFlight += 1;
                maxInFlight = Math.max(maxInFlight, inFlight);
                await new Promise(r => setTimeout(r, 1));
                inFlight -= 1;
                return true;
            });
            const { result } = setup(factory(), writeParam);

            await act(async () => {
                await result.current.applyPreset(loudClub);
            });
            expect(maxInFlight).toBe(1);
        });

        it("reports which parameters failed instead of claiming success", async () => {
            const writeParam = jest
                .fn()
                .mockResolvedValueOnce(true)
                .mockResolvedValueOnce(false)
                .mockResolvedValue(true);
            const { result } = setup(factory(), writeParam);

            let out: { applied: number; failed: AudioParamKey[] } | undefined;
            await act(async () => {
                out = await result.current.applyPreset(loudClub);
            });
            expect(out!.failed).toHaveLength(1);
            expect(out!.applied).toBe(Object.keys(loudClub.values).length - 1);
        });
    });

    describe("undo", () => {
        it("restores exactly the parameters the change touched", async () => {
            const current = factory();
            const { result, writeParam } = setup(current);

            await act(async () => {
                await result.current.applyPreset(loudClub);
            });
            expect(result.current.canUndo).toBe(true);
            writeParam.mockClear();

            await act(async () => {
                await result.current.undo();
            });

            const touched = Object.keys(loudClub.values) as AudioParamKey[];
            expect(writeParam).toHaveBeenCalledTimes(touched.length);
            touched.forEach(key => {
                expect(writeParam).toHaveBeenCalledWith(key, current[key]);
            });
        });

        it("is a no-op with nothing to undo", async () => {
            const { result, writeParam } = setup();
            await act(async () => {
                const out = await result.current.undo();
                expect(out.applied).toBe(0);
            });
            expect(writeParam).not.toHaveBeenCalled();
            expect(result.current.canUndo).toBe(false);
        });

        it("pops the entry even when the restore partly fails", async () => {
            // Leaving it on top would invite a blind retry against state that has already moved.
            const writeParam = jest.fn().mockResolvedValue(true);
            const { result } = setup(factory(), writeParam);
            await act(async () => {
                await result.current.applyPreset(loudClub);
            });

            writeParam.mockResolvedValue(false);
            await act(async () => {
                await result.current.undo();
            });
            expect(result.current.canUndo).toBe(false);
        });

        it("keeps only the most recent UNDO_DEPTH changes", async () => {
            const { result } = setup();
            await act(async () => {
                for (let i = 0; i < UNDO_DEPTH + 3; i++) {
                    await result.current.applyValues(`change ${i}`, [
                        { key: "beatAlpha", to: 0.2 + i * 0.01 },
                    ]);
                }
            });
            expect(result.current.undoStack).toHaveLength(UNDO_DEPTH);
            expect(result.current.undoStack[0].label).toBe(`change ${UNDO_DEPTH + 2}`);
        });
    });

    describe("saving", () => {
        it("captures the current values and persists them", async () => {
            const { result } = setup();
            act(() => {
                result.current.saveCurrentAs("Warehouse", 1234);
            });

            await waitFor(() => expect(result.current.saved).toHaveLength(1));
            expect(result.current.saved[0].name).toBe("Warehouse");
            expect(savePresets).toHaveBeenCalled();
        });

        it("persists the NEW list, not an empty one", () => {
            // Persistence used to read the list out of a setState updater. React does not promise
            // to run that synchronously, so on device savePresets wrote `[]` while the UI reported
            // success — a Pixel 9 Pro produced {"version":1,"presets":[]} on disk.
            //
            // Be honest about what this test does and does not do: jest's act() flushes updaters
            // synchronously, so it would have PASSED against the buggy code too. The real fix is
            // structural — persistence now reads from a ref and never from an updater. What this
            // pins is the contract (the persisted list is the new one), so a future refactor back
            // toward updater-derived state at least has to change a test that says otherwise.
            // The original defect was only observable on device.
            const { result } = setup();
            act(() => {
                result.current.saveCurrentAs("Warehouse", 1234);
            });

            const written = (savePresets as jest.Mock).mock.calls.at(-1)![0] as { name: string }[];
            expect(written).toHaveLength(1);
            expect(written[0].name).toBe("Warehouse");
        });


        it("does not clobber a preset another instance saved after this one mounted", () => {
        // The calibration wizard saves its "Before calibration" rescue preset while the tuning
        // screen below stays mounted with a savedRef seeded before that preset existed. Every
        // commit writes the WHOLE list, so a mutation computed from the stale ref erased the
        // one documented way back from a bad calibration. Re-reading before mutating makes
        // each commit last-writer-wins per ENTRY rather than per LIST.
        const a = renderHook(() =>
            useAudioPresets({ currentValues: factory(), writeParam: jest.fn() }),
        );
        act(() => {
            a.result.current.saveCurrentAs("From screen A", 1);
        });

        // A second instance mounts and saves something A has never seen.
        const b = renderHook(() =>
            useAudioPresets({ currentValues: factory(), writeParam: jest.fn() }),
        );
        act(() => {
            b.result.current.saveCurrentAs("Before calibration", 2);
        });

        // A now saves again from its stale view. Both must survive.
        act(() => {
            a.result.current.saveCurrentAs("From screen A again", 3);
        });

        const written = (savePresets as jest.Mock).mock.calls.at(-1)![0] as {
            name: string;
        }[];
        expect(written.map(p => p.name).sort()).toEqual([
            "Before calibration",
            "From screen A",
            "From screen A again",
        ]);
    });

        it("persists the reduced list when deleting", () => {
            const { result } = setup();
            act(() => {
                result.current.saveCurrentAs("One", 1);
            });
            act(() => {
                result.current.saveCurrentAs("Two", 2);
            });
            expect((savePresets as jest.Mock).mock.calls.at(-1)![0]).toHaveLength(2);

            const id = result.current.saved.find(p => p.name === "One")!.id;
            act(() => {
                result.current.deletePreset(id);
            });

            const written = (savePresets as jest.Mock).mock.calls.at(-1)![0] as { name: string }[];
            expect(written).toHaveLength(1);
            expect(written[0].name).toBe("Two");
        });

        it("overwrites a same-named preset rather than accumulating duplicates", async () => {
            const { result } = setup();
            act(() => {
                result.current.saveCurrentAs("Warehouse", 1);
            });
            await waitFor(() => expect(result.current.saved).toHaveLength(1));
            act(() => {
                result.current.saveCurrentAs("Warehouse", 2);
            });
            await waitFor(() => expect(result.current.saved[0].savedAt).toBe(2));
            expect(result.current.saved).toHaveLength(1);
        });

        it("deletes a preset and clears it from any A/B slot", async () => {
            const { result } = setup();
            act(() => {
                result.current.saveCurrentAs("Warehouse", 7);
            });
            await waitFor(() => expect(result.current.saved).toHaveLength(1));

            const id = result.current.saved[0].id;
            act(() => {
                result.current.setSlotA(id);
            });
            await waitFor(() => expect(result.current.slotA).toBe(id));

            act(() => {
                result.current.deletePreset(id);
            });
            await waitFor(() => expect(result.current.saved).toHaveLength(0));
            expect(result.current.slotA).toBeNull();
        });
    });

    describe("A/B swap", () => {
        it("does nothing until both slots are assigned", async () => {
            const { result, writeParam } = setup();
            act(() => {
                result.current.setSlotA("builtin:factory");
            });
            await act(async () => {
                expect(await result.current.swapAB()).toEqual({ kind: "unassigned" });
            });
            expect(writeParam).not.toHaveBeenCalled();
        });

        it("moves to the other preset when the device already matches one slot", async () => {
            // Device is on factory (slot A), so a swap must go to B.
            const { result, writeParam } = setup();
            act(() => {
                result.current.setSlotA("builtin:factory");
                result.current.setSlotB("builtin:loud-club");
            });
            await waitFor(() => expect(result.current.slotB).toBe("builtin:loud-club"));

            await act(async () => {
                await result.current.swapAB();
            });

            const written = writeParam.mock.calls.map(c => c[0]);
            expect(written.length).toBe(Object.keys(loudClub.values).length);
            expect(written).toContain("agcNoiseGateRms");
        });

        it("goes to A when the device matches NEITHER slot", async () => {
            // The old comment promised B for this case; the code went to A. A compare button has
            // to be predictable, so A it is - asserted, rather than left to whichever the diff
            // happened to favour.
            const drifted = { ...factory(), agcNoiseGateRms: 0.00321, beatRefractoryFrames: 9 };
            const { result } = setup(drifted);
            act(() => {
                result.current.setSlotA("builtin:speech");
                result.current.setSlotB("builtin:loud-club");
            });
            await waitFor(() => expect(result.current.slotB).toBe("builtin:loud-club"));

            let outcome: Awaited<ReturnType<typeof result.current.swapAB>> | undefined;
            await act(async () => {
                outcome = await result.current.swapAB();
            });

            expect(outcome).toMatchObject({ kind: "applied" });
            expect(outcome!.kind === "applied" && outcome!.preset.id).toBe("builtin:speech");
        });

        it("reports `identical` instead of announcing a swap that writes nothing", async () => {
            /* Presets are PARTIAL maps, so a device can match both slots on every key either one
             * expresses. That used to return an ApplyResult of 0 writes, which the screen reported
             * as "Swapped (0 changed)" - success for something that did nothing and could not do
             * anything until a slot was reassigned. */
            const both = { ...factory(), ...loudClub.values };
            const { result, writeParam } = setup(both);
            act(() => {
                result.current.setSlotA("builtin:loud-club");
                result.current.setSlotB("builtin:loud-club");
            });
            await waitFor(() => expect(result.current.slotB).toBe("builtin:loud-club"));

            await act(async () => {
                expect(await result.current.swapAB()).toEqual({ kind: "identical" });
            });
            expect(writeParam).not.toHaveBeenCalled();
        });
    });

    describe("failure handling", () => {
        it("leaves NO undo entry when every write failed", async () => {
            /* The entry used to be pushed before the writes and never removed, so five failed
             * applies flushed a real pre-experiment snapshot off a five-deep stack. */
            const { result } = setup(factory(), jest.fn().mockResolvedValue(false));
            await act(async () => {
                await result.current.applyPreset(loudClub);
            });
            expect(result.current.undoStack).toHaveLength(0);
            expect(result.current.canUndo).toBe(false);
        });

        it("does not evict real history behind a run of failed applies", async () => {
            const writeParam = jest.fn().mockResolvedValue(true);
            const { result } = setup(factory(), writeParam);

            await act(async () => {
                await result.current.applyValues("Hand tuned", [
                    { key: "beatRefractoryFrames", to: 9 },
                ]);
            });
            await waitFor(() => expect(result.current.undoStack).toHaveLength(1));

            writeParam.mockResolvedValue(false);
            for (let i = 0; i < UNDO_DEPTH; i++) {
                await act(async () => {
                    await result.current.applyPreset(loudClub);
                });
            }

            expect(result.current.undoStack).toHaveLength(1);
            expect(result.current.undoStack[0].label).toBe("Hand tuned");
        });

        it("records an undo entry covering only the writes that landed", async () => {
            const writeParam = jest
                .fn()
                .mockResolvedValueOnce(true)
                .mockResolvedValue(false);
            const { result } = setup(factory(), writeParam);

            await act(async () => {
                await result.current.applyPreset(loudClub);
            });

            await waitFor(() => expect(result.current.undoStack).toHaveLength(1));
            const keys = Object.keys(result.current.undoStack[0].values);
            expect(keys).toHaveLength(1);
            expect(writeParam.mock.calls[0][0]).toBe(keys[0]);
        });

        it("reports a failed disk write rather than claiming the preset was saved", async () => {
            (savePresets as jest.Mock).mockReturnValueOnce(false);
            const { result } = setup();
            let saved: ReturnType<typeof result.current.saveCurrentAs>;
            act(() => {
                saved = result.current.saveCurrentAs("Warehouse", 1);
            });
            expect(saved!).not.toBeNull();
            expect(saved!!.persisted).toBe(false);
        });

        it("says whether a save added a preset or replaced one", async () => {
            const { result } = setup();
            let first: ReturnType<typeof result.current.saveCurrentAs>;
            act(() => {
                first = result.current.saveCurrentAs("Warehouse", 1);
            });
            await waitFor(() => expect(result.current.saved).toHaveLength(1));
            expect(first!!.replaced).toBe(false);

            let second: ReturnType<typeof result.current.saveCurrentAs>;
            act(() => {
                second = result.current.saveCurrentAs("Warehouse", 2);
            });
            await waitFor(() => expect(result.current.saved[0].savedAt).toBe(2));
            expect(second!!.replaced).toBe(true);
        });

        it("reports a delete that did not reach disk", async () => {
            const { result } = setup();
            act(() => {
                result.current.saveCurrentAs("Warehouse", 7);
            });
            await waitFor(() => expect(result.current.saved).toHaveLength(1));

            (savePresets as jest.Mock).mockReturnValueOnce(false);
            let persisted = true;
            act(() => {
                persisted = result.current.deletePreset(result.current.saved[0].id);
            });
            expect(persisted).toBe(false);
        });
    });

    describe("render stability", () => {
        it("returns the same object across renders that change nothing", () => {
            const currentValues = factory();
            const writeParam = jest.fn().mockResolvedValue(true);
            const { result, rerender } = renderHook(() =>
                useAudioPresets({ currentValues, writeParam }),
            );

            const first = result.current;
            rerender({});
            /* Kill this by dropping the useMemo around the hook's return: a fresh literal per
             * render is what made audio.tsx's changeCounts memo never once hit, re-diffing every
             * preset against all 14 parameters on every slider-drag frame. */
            expect(result.current).toBe(first);
        });
    });
});
