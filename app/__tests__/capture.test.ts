import {
  CAPTURE_MAX_LIMIT_S,
  CAPTURE_STATE_FAILED,
  CAPTURE_STATE_IDLE,
  CAPTURE_STATE_RECORDING,
  canStartCapture,
  captureProgress,
  captureStateFromCode,
  captureStateLabel,
  captureStateTone,
  effectiveLimitSeconds,
  formatCaptureDuration,
} from '@/services/capture';

describe('captureStateFromCode', () => {
  it('maps the firmware enum values', () => {
    expect(captureStateFromCode(CAPTURE_STATE_IDLE)).toBe('idle');
    expect(captureStateFromCode(CAPTURE_STATE_RECORDING)).toBe('recording');
    expect(captureStateFromCode(CAPTURE_STATE_FAILED)).toBe('failed');
  });

  it('does not invent a state for an absent or unknown value', () => {
    // A missing read must not render as "Ready" — that would claim the device is
    // idle when the app simply has not heard from it yet.
    expect(captureStateFromCode(null)).toBe('unknown');
    expect(captureStateFromCode(undefined)).toBe('unknown');
    expect(captureStateFromCode(99)).toBe('unknown');
  });

  it('labels and tones every state', () => {
    expect(captureStateLabel('recording')).toBe('Recording');
    expect(captureStateTone('recording')).toBe('danger');
    expect(captureStateLabel('failed')).toBe('Failed');
    expect(captureStateTone('failed')).toBe('warning');
    expect(captureStateLabel('idle')).toBe('Ready');
    expect(captureStateTone('idle')).toBe('success');
    expect(captureStateTone('unknown')).toBe('neutral');
  });
});

describe('formatCaptureDuration', () => {
  it('renders m:ss', () => {
    expect(formatCaptureDuration(0)).toBe('0:00');
    expect(formatCaptureDuration(7)).toBe('0:07');
    expect(formatCaptureDuration(65)).toBe('1:05');
    expect(formatCaptureDuration(180)).toBe('3:00');
  });

  it('distinguishes "unknown" from zero', () => {
    expect(formatCaptureDuration(null)).toBe('—');
    expect(formatCaptureDuration(undefined)).toBe('—');
    expect(formatCaptureDuration(NaN)).toBe('—');
    expect(formatCaptureDuration(-1)).toBe('—');
  });
});

describe('effectiveLimitSeconds', () => {
  it('mirrors the firmware clamp: a request larger than the free space records the free space', () => {
    // capture_start() clamps rather than failing — a field user on a nearly-full
    // disk is better served by a short capture than an error, so the UI has to say
    // what will actually happen before the button is pressed.
    expect(effectiveLimitSeconds(60, 20)).toBe(20);
    expect(effectiveLimitSeconds(60, 120)).toBe(60);
  });

  it('treats 0/absent as "as long as the volume allows"', () => {
    expect(effectiveLimitSeconds(0, 45)).toBe(45);
    expect(effectiveLimitSeconds(null, 45)).toBe(45);
  });

  it('never exceeds the firmware ceiling even on an empty volume', () => {
    expect(effectiveLimitSeconds(0, 9999)).toBe(CAPTURE_MAX_LIMIT_S);
    expect(effectiveLimitSeconds(600, 9999)).toBe(CAPTURE_MAX_LIMIT_S);
  });

  it('is null when nothing can be recorded', () => {
    expect(effectiveLimitSeconds(30, 0)).toBeNull();
    expect(effectiveLimitSeconds(30, null)).toBeNull();
  });
});

describe('canStartCapture', () => {
  it('requires known, non-zero room', () => {
    expect(canStartCapture(1)).toBe(true);
    expect(canStartCapture(0)).toBe(false);
    expect(canStartCapture(null)).toBe(false);
  });
});

describe('captureProgress', () => {
  it('is a 0..1 fraction of the effective limit', () => {
    expect(captureProgress(0, 30)).toBe(0);
    expect(captureProgress(15, 30)).toBe(0.5);
    expect(captureProgress(30, 30)).toBe(1);
  });

  it('cannot produce NaN or overshoot while values are still being seeded', () => {
    expect(captureProgress(5, 0)).toBe(0);
    expect(captureProgress(5, null)).toBe(0);
    expect(captureProgress(null, 30)).toBe(0);
    // Elapsed can exceed the limit by a tick: the device stops on its own sample
    // count, not on the second boundary the UI is counting.
    expect(captureProgress(31, 30)).toBe(1);
  });
});
