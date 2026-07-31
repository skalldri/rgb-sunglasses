import {
    BLE_GATT_CPF_FORMAT_SLOT_NOW_PLAYING, BLE_GATT_CPF_FORMAT_SLOT_TEXT,
    BLE_GATT_CPF_FORMAT_SLOT_UP_NEXT,
} from "@/constants/bluetooth";
import { CharacteristicInfo } from "@/context/bluetooth-context";
import { decodeUint32FromBase64 } from "@/services/ble-value-codec";

/**
 * Grouping logic for the generic slot-playlist contract (issue #260): any service exposing
 * SLOT_TEXT characteristics (plus optionally one SLOT_UP_NEXT and one SLOT_NOW_PLAYING) gets
 * a playlist-style UI — one row per slot with a tap-to-queue button and a now-playing
 * highlight — instead of raw text/number inputs. Pure functions so this is unit-testable
 * without rendering.
 *
 * ORDERING ASSUMPTION: slot index = 0-based order of appearance of the SLOT_TEXT
 * characteristics in the service's characteristic map. That map is built in discovery order
 * (hooks/use-ble-connection.ts) and JS objects preserve string-key insertion order, so
 * appearance order == ATT ascending-handle order == firmware declaration order — the same
 * chain the bulk-metadata positional zip relies on (see the ordering-assumption comments in
 * use-ble-connection.ts and fw/src/bluetooth/bt_service_cpp.h, including the one
 * verified-but-not-enforced link: ble-plx passing Android's native discovery order through
 * unmodified). A same-count reorder would silently mis-index slots — the same accepted
 * residual risk as the metadata zip.
 */

export interface SlotEntry {
    charUuid: string;
    charInfo: CharacteristicInfo;
    // 0-based ordinal among the service's SLOT_TEXT characteristics — the index the
    // SLOT_UP_NEXT / SLOT_NOW_PLAYING characteristics refer to on the wire.
    slotIndex: number;
}

export interface SlotPlaylist {
    slots: SlotEntry[];
    upNext: { charUuid: string; charInfo: CharacteristicInfo } | null;
    nowPlaying: { charUuid: string; charInfo: CharacteristicInfo } | null;
    // Characteristics absorbed into the playlist UI, to be excluded from the generic
    // per-characteristic row loop: every slot, plus the first upNext/nowPlaying.
    hiddenCharUuids: Set<string>;
}

/**
 * Returns the service's slot playlist, or null when the service has no SLOT_TEXT
 * characteristics — in which case the screen renders exactly as before (a stray
 * SLOT_UP_NEXT/SLOT_NOW_PLAYING falls back to its standalone numeric/readonly row via
 * renderCharacteristicInput's uint32-alias handling).
 *
 * A missing upNext or nowPlaying is tolerated (null) — slot rows still render, minus that
 * affordance. If a service pathologically exposes duplicates, the first wins and the
 * extras stay visible as standalone rows (deterministic, never silently dropped).
 */
export function groupSlotPlaylist(
    chars: Record<string, CharacteristicInfo>,
): SlotPlaylist | null {
    const slots: SlotEntry[] = [];
    let upNext: SlotPlaylist["upNext"] = null;
    let nowPlaying: SlotPlaylist["nowPlaying"] = null;

    // Plain loop (not forEach) so TS control-flow analysis tracks the upNext/nowPlaying
    // assignments — assignments inside a callback narrow them back to their initial null.
    for (const [charUuid, charInfo] of Object.entries(chars)) {
        if (charInfo.cpfFormat === BLE_GATT_CPF_FORMAT_SLOT_TEXT) {
            slots.push({ charUuid, charInfo, slotIndex: slots.length });
        } else if (charInfo.cpfFormat === BLE_GATT_CPF_FORMAT_SLOT_UP_NEXT) {
            if (!upNext) upNext = { charUuid, charInfo };
        } else if (charInfo.cpfFormat === BLE_GATT_CPF_FORMAT_SLOT_NOW_PLAYING) {
            if (!nowPlaying) nowPlaying = { charUuid, charInfo };
        }
    }

    if (slots.length === 0) {
        return null;
    }

    const hiddenCharUuids = new Set<string>(slots.map((s) => s.charUuid));
    if (upNext) hiddenCharUuids.add(upNext.charUuid);
    if (nowPlaying) hiddenCharUuids.add(nowPlaying.charUuid);

    return { slots, upNext, nowPlaying, hiddenCharUuids };
}

/**
 * Decodes a SLOT_UP_NEXT / SLOT_NOW_PLAYING characteristic's current value to a slot index,
 * or null when there's no value or it doesn't decode as a uint32 (short/garbage payload).
 */
export function decodeSlotIndex(charInfo: CharacteristicInfo | null | undefined): number | null {
    if (!charInfo?.value) return null;
    try {
        return decodeUint32FromBase64(charInfo.value);
    } catch {
        return null;
    }
}
