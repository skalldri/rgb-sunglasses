/**
 * Tests for on-disk preset persistence.
 *
 * The store is deliberately total: every failure path degrades to "no saved presets" and logs,
 * because losing saved presets is annoying but throwing on the Controls tab because a JSON file
 * got truncated is worse. These tests pin that, and pin the sanitisation that stops a bad file
 * putting an out-of-range value onto the device.
 */

const mockFile = {
    exists: true,
    textSync: jest.fn(),
    write: jest.fn(),
    create: jest.fn(),
};

jest.mock("expo-file-system/next", () => ({
    File: jest.fn(() => mockFile),
    Paths: { document: "/documents" },
}));

import { AUDIO_PARAMS } from "@/services/audio-params";
import {
    AUDIO_PRESET_STORE_VERSION,
    loadPresets,
    savePresets,
} from "@/services/audio-preset-store";
import { AudioPreset } from "@/services/audio-presets";

const wrap = (presets: unknown[]) =>
    JSON.stringify({ version: AUDIO_PRESET_STORE_VERSION, presets });

describe("loadPresets", () => {
    beforeEach(() => {
        jest.clearAllMocks();
        mockFile.exists = true;
        jest.spyOn(console, "log").mockImplementation(() => {});
    });
    afterEach(() => jest.restoreAllMocks());

    it("returns an empty list when the file does not exist yet", () => {
        mockFile.exists = false;
        expect(loadPresets()).toEqual([]);
    });

    it("round-trips a valid preset", () => {
        mockFile.textSync.mockReturnValue(
            wrap([{ id: "saved:1", name: "Warehouse", values: { beatAlpha: 0.5 }, savedAt: 1 }]),
        );
        const out = loadPresets();
        expect(out).toHaveLength(1);
        expect(out[0].name).toBe("Warehouse");
        expect(out[0].values.beatAlpha).toBe(0.5);
    });

    it("degrades to empty on corrupt JSON rather than throwing", () => {
        mockFile.textSync.mockReturnValue("{not json");
        expect(loadPresets()).toEqual([]);
    });

    it("degrades to empty when the read itself throws", () => {
        mockFile.textSync.mockImplementation(() => {
            throw new Error("EIO");
        });
        expect(loadPresets()).toEqual([]);
    });

    it("discards an unknown store version instead of guessing at its shape", () => {
        mockFile.textSync.mockReturnValue(
            JSON.stringify({ version: 999, presets: [{ id: "a", name: "b", values: { beatAlpha: 1 } }] }),
        );
        expect(loadPresets()).toEqual([]);
    });

    describe("sanitisation", () => {
        it("clamps an out-of-range value to the metadata table", () => {
            // Without this, applying the preset would write a value the firmware silently
            // clamps, leaving the user on a setting the preset does not describe.
            mockFile.textSync.mockReturnValue(
                wrap([{ id: "s", name: "bad", values: { beatAlpha: 9999 } }]),
            );
            expect(loadPresets()[0].values.beatAlpha).toBe(AUDIO_PARAMS.beatAlpha.max);
        });

        it("drops unknown parameter keys", () => {
            mockFile.textSync.mockReturnValue(
                wrap([{ id: "s", name: "x", values: { beatAlpha: 0.5, notAParam: 3 } }]),
            );
            const values = loadPresets()[0].values as Record<string, number>;
            expect(values.notAParam).toBeUndefined();
            expect(values.beatAlpha).toBe(0.5);
        });

        it("drops non-finite values", () => {
            mockFile.textSync.mockReturnValue(
                wrap([{ id: "s", name: "x", values: { beatAlpha: null, agcAttackFrames: 3 } }]),
            );
            const out = loadPresets()[0];
            expect(out.values.beatAlpha).toBeUndefined();
            expect(out.values.agcAttackFrames).toBe(3);
        });

        it("demotes anything on disk claiming to be built-in", () => {
            // A "built-in" loaded from disk could never be deleted through the UI.
            mockFile.textSync.mockReturnValue(
                wrap([{ id: "s", name: "x", builtIn: true, values: { beatAlpha: 0.5 } }]),
            );
            expect(loadPresets()[0].builtIn).toBe(false);
        });

        it("drops entries with no usable values or no identity at all", () => {
            mockFile.textSync.mockReturnValue(
                wrap([
                    { id: "s", name: "empty", values: {} },
                    { id: "", name: "no id", values: { beatAlpha: 1 } },
                    { name: "no id field", values: { beatAlpha: 1 } },
                    null,
                    "nonsense",
                ]),
            );
            expect(loadPresets()).toEqual([]);
        });
    });
});

describe("savePresets", () => {
    beforeEach(() => {
        jest.clearAllMocks();
        mockFile.exists = true;
        jest.spyOn(console, "log").mockImplementation(() => {});
    });
    afterEach(() => jest.restoreAllMocks());

    it("writes only user presets, never the built-ins", () => {
        // Built-ins live in code; persisting them would freeze a stale copy that survives a
        // firmware retune.
        const presets: AudioPreset[] = [
            { id: "builtin:factory", name: "Factory defaults", builtIn: true, values: { beatAlpha: 0.3 } },
            { id: "saved:1", name: "Mine", builtIn: false, values: { beatAlpha: 0.5 } },
        ];
        expect(savePresets(presets)).toBe(true);

        const written = JSON.parse(mockFile.write.mock.calls[0][0]);
        expect(written.version).toBe(AUDIO_PRESET_STORE_VERSION);
        expect(written.presets).toHaveLength(1);
        expect(written.presets[0].name).toBe("Mine");
    });

    it("creates the file when it is missing", () => {
        mockFile.exists = false;
        expect(savePresets([])).toBe(true);
        expect(mockFile.create).toHaveBeenCalled();
    });

    it("reports failure rather than throwing when the write fails", () => {
        // The caller needs to be able to tell the user the preset did not save.
        mockFile.write.mockImplementation(() => {
            throw new Error("ENOSPC");
        });
        expect(savePresets([])).toBe(false);
    });
});
