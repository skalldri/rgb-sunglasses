/**
 * Pure model for the firmware's Capture service (fw/src/sound/capture.cpp +
 * fw/src/bluetooth/capture_service.cpp): on-device audio + IMU recording, started
 * and stopped from the phone so a stimulus can be captured in the field.
 *
 * The app never transfers the files — captures accumulate on /NAND: under
 * auto-indexed names and are collected later over USB mass storage, which is why
 * "how many are waiting" and "how much room is left" are first-class readouts
 * here rather than an afterthought.
 */

/** Mirrors `enum capture_state` in fw/src/sound/capture.h. */
export const CAPTURE_STATE_IDLE = 0;
export const CAPTURE_STATE_RECORDING = 1;
export const CAPTURE_STATE_FAILED = 2;

/** Mirrors the command values `CaptureControlCharacteristic::onWriteChecked` accepts. */
export const CAPTURE_COMMAND_STOP = 0;
export const CAPTURE_COMMAND_START = 1;

/**
 * Hard ceiling the firmware clamps every request to (`kMaxLimitS`). It is close to
 * the physical ceiling anyway: the volume holds roughly three minutes of audio.
 */
export const CAPTURE_MAX_LIMIT_S = 180;

/** Lengths offered in the UI. Anything longer than the volume can hold is clamped device-side. */
export const CAPTURE_LENGTH_PRESETS_S = [10, 20, 30, 60, 120];

export type CaptureState = 'idle' | 'recording' | 'failed' | 'unknown';

/** Decodes the raw uint32 the Capture State characteristic carries. */
export function captureStateFromCode(code: number | null | undefined): CaptureState {
    switch (code) {
        case CAPTURE_STATE_IDLE: return 'idle';
        case CAPTURE_STATE_RECORDING: return 'recording';
        case CAPTURE_STATE_FAILED: return 'failed';
        default: return 'unknown';
    }
}

export function captureStateLabel(state: CaptureState): string {
    switch (state) {
        case 'idle': return 'Ready';
        case 'recording': return 'Recording';
        case 'failed': return 'Failed';
        default: return 'Unknown';
    }
}

export function captureStateTone(state: CaptureState): 'success' | 'danger' | 'warning' | 'neutral' {
    switch (state) {
        case 'recording': return 'danger';   // red, matching a record indicator
        case 'failed': return 'warning';
        case 'idle': return 'success';
        default: return 'neutral';
    }
}

/**
 * m:ss for anything under an hour, which every capture is (the ceiling is 180 s).
 * Null/undefined renders as an em dash rather than "0:00" — "we don't know yet" and
 * "zero" are different things on a screen that is reporting device state.
 */
export function formatCaptureDuration(seconds: number | null | undefined): string {
    if (seconds == null || !Number.isFinite(seconds) || seconds < 0) return '—';
    const whole = Math.floor(seconds);
    const minutes = Math.floor(whole / 60);
    const rest = whole % 60;
    return `${minutes}:${rest.toString().padStart(2, '0')}`;
}

/**
 * What the device will actually record, mirroring `capture_start()`'s clamp: a
 * request of 0, or one larger than the free space can hold, becomes "all the room
 * there is". Shown up front so a field user is never surprised by a capture that
 * stops earlier than the length they picked.
 */
export function effectiveLimitSeconds(
    requestedS: number | null | undefined,
    remainingS: number | null | undefined,
): number | null {
    if (remainingS == null || remainingS <= 0) return null;   // nothing can be recorded
    const room = Math.min(remainingS, CAPTURE_MAX_LIMIT_S);
    if (requestedS == null || requestedS <= 0) return room;
    return Math.min(requestedS, room);
}

/** A start is only offered when the volume can still hold something worth recording. */
export function canStartCapture(remainingS: number | null | undefined): boolean {
    return remainingS != null && remainingS > 0;
}

/**
 * Progress of the running capture, 0..1. Guards a zero/absent limit so the bar
 * cannot render NaN while the app is still seeding its reads.
 */
export function captureProgress(
    elapsedS: number | null | undefined,
    limitS: number | null | undefined,
): number {
    if (elapsedS == null || limitS == null || limitS <= 0) return 0;
    return Math.max(0, Math.min(1, elapsedS / limitS));
}
