/**
 * Human-readable message for a failed BLE characteristic write.
 *
 * react-native-ble-plx surfaces a `BleError` (or, defensively, anything) in the write helpers'
 * catch. The firmware convention (see fw/CLAUDE.md `bt_service_cpp.h`) is to refuse an
 * unacceptable write with an ATT error — most importantly `BT_ATT_ERR_WRITE_REQ_REJECTED`
 * (ATT code 0xFC / 252), e.g. when activating a faulted extension. We map that specific case to a
 * friendly line and fall back to a generic message otherwise. See issue #92.
 *
 * The exact field ble-plx populates for the ATT reason differs by platform. Hardware-verified on
 * Android (issue #92): a refused write surfaces as `androidErrorCode: 252` (0xFC) with
 * `attErrorCode: null` and a `reason` string like "...status 252 (UNKNOWN)... Look up status 0xfc
 * here...". iOS would populate `attErrorCode`/`iosErrorCode` instead. The structured numeric fields
 * are authoritative; we only fall back to the reason text for builds that don't populate them, and
 * then only on the specific 0xFC/252 status as a standalone token — matching loose substrings (a
 * stray "reject" word, a hex fragment) would mislabel an unrelated failure (disconnect, timeout) as
 * a device refusal, so we deliberately do not.
 */
const ATT_ERR_WRITE_REQ_REJECTED = 0xfc; // 252

// The 0xFC/252 status as a standalone token, so unrelated hex/status fragments don't false-positive.
const REJECTED_REASON_RE = /\bstatus 252\b|\b0xfc\b/i;

const REJECTED_MESSAGE = 'The device refused the change.';
const GENERIC_MESSAGE = 'Write failed.';

export function describeWriteError(error: unknown): string {
    if (error && typeof error === 'object') {
        const e = error as { attErrorCode?: unknown; androidErrorCode?: unknown; reason?: unknown };
        // Authoritative: Android puts the ATT status in androidErrorCode; iOS/other in attErrorCode.
        if (e.attErrorCode === ATT_ERR_WRITE_REQ_REJECTED || e.androidErrorCode === ATT_ERR_WRITE_REQ_REJECTED) {
            return REJECTED_MESSAGE;
        }
        // Fallback only for builds that surface the status solely in the reason text.
        if (typeof e.reason === 'string' && REJECTED_REASON_RE.test(e.reason)) {
            return REJECTED_MESSAGE;
        }
    }

    return GENERIC_MESSAGE;
}
