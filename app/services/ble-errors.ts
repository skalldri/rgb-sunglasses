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

/**
 * Human-readable message for a failed CONNECT attempt (`connect()` in hooks/use-ble-connection.ts).
 *
 * Why this exists: connect() resolves `false` on every failure and the caller simply doesn't
 * navigate, so a failed connect was indistinguishable from a dead button — no spinner change, no
 * message, nothing. That silence is what made a bond mismatch (below) read as "the app release
 * broke Android" rather than "this phone needs re-pairing" (2026-08-14 investigation).
 *
 * Deliberately NOT classified by error code. The board-side signature of the common failure is
 * unambiguous — `Security failed ... err 2` (BT_SECURITY_ERR_PIN_OR_KEY_MISSING), i.e. the phone
 * offers bonding keys the device no longer has, after which the firmware drops the link because it
 * requires L4 — but the corresponding ble-plx error shape on the APP side has not been captured on
 * hardware. describeWriteError above only asserts `androidErrorCode: 252` because that was measured
 * (issue #92); inventing a connect-side equivalent from inference would be the same class of
 * mistake this file's comments warn about. So we surface the underlying reason verbatim and let the
 * user read it, plus a hint that covers the failure mode without claiming to have detected it.
 *
 * If the pairing case is ever measured, add a narrow structured-field check here (never a loose
 * substring) and return a specific message for it.
 */
const CONNECT_FAILED_MESSAGE = 'Could not connect.';

// The pairing/bond-mismatch case cannot currently be distinguished from a generic failure, so this
// is phrased as guidance rather than a diagnosis. It is the single most likely cause of a connect
// that fails immediately, and the recovery (forget + re-pair) is harmless if the cause was
// something else.
export const CONNECT_FAILED_HINT =
    'If this keeps happening, forget the device in your phone\'s Bluetooth settings and connect ' +
    'again — the glasses can lose their pairing keys (for example after a firmware re-flash), and ' +
    'Android keeps the stale pairing instead of re-pairing automatically.';

export function describeConnectError(error: unknown): string {
    if (error && typeof error === 'object') {
        const e = error as { reason?: unknown; message?: unknown };
        // ble-plx populates `reason` with the native stack's text (the most specific thing
        // available); Error.message is the fallback for a plain throw.
        const detail = typeof e.reason === 'string' && e.reason
            ? e.reason
            : typeof e.message === 'string' && e.message
                ? e.message
                : null;
        if (detail) {
            return `${CONNECT_FAILED_MESSAGE} ${detail}`;
        }
    }

    return CONNECT_FAILED_MESSAGE;
}
