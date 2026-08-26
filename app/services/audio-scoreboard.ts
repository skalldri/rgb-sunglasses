import { AUDIO_PARAMS } from "@/services/audio-params";
import type { TelemetrySummary } from "@/services/audio-telemetry";

/**
 * Turns a telemetry summary into one sentence a person in a loud dark room can act on.
 *
 * Design rules, all of them learned from the plan's field notes rather than invented:
 *
 * - EXACTLY ONE verdict at a time, chosen by priority. Showing three simultaneous problems
 *   to someone holding a phone one-handed at a gig is the same as showing none.
 * - Every verdict names the control to change, in the words the Simple tab uses for it.
 *   "Clipping detected" is a diagnosis; "lower Loudest we want" is a fix.
 * - Colour is never the only signal (dark venue, and colour-blind users) — `tone` is paired
 *   with text that stands alone.
 * - The copy is product surface, so it is pinned by tests. Changing a sentence should be a
 *   deliberate diff, not a silent drift.
 *
 * The ordering is the interesting part: it is not by severity but by WHICH FIX COMES FIRST.
 * A muted stream and an insensitive detector look identical on the meters, but turning
 * sensitivity up while the noise gate is eating the music does nothing — so `muted` must
 * outrank `insensitive`. Likewise a pinned mic outranks sensitivity advice, because no
 * amount of sensitivity fixes a room that is too quiet where the glasses are.
 */

export type VerdictTone = "neutral" | "good" | "warning" | "bad";

export type VerdictKind =
  | "no-data"
  | "stale"
  | "muted"
  | "clipping"
  | "gain-pinned"
  | "too-sensitive"
  | "insensitive"
  | "good";

export type Verdict = {
  kind: VerdictKind;
  tone: VerdictTone;
  /** Short headline. */
  title: string;
  /** One sentence naming the control to change. */
  detail: string;
};

/** Fraction of the window spent below the noise gate before we call it a problem. */
export const MUTED_FRACTION = 0.3;
/** Fraction of frames clipping before we say so. The AGC backs off on its own below this. */
export const CLIP_FRACTION = 0.05;
/** Above this, the detector is firing on snares and hi-hats, not beats. */
export const TOO_SENSITIVE_BPS = 6;
/** Below this, with real signal present, nothing is firing. */
export const INSENSITIVE_BPS = 0.2;
/** Ignore the insensitive verdict until the window is this full — it needs real evidence. */
const MIN_FRAMES_FOR_SENSITIVITY = 8;

function pct(fraction: number): number {
  return Math.round(fraction * 100);
}

/**
 * How the surrounding screen is presenting sensitivity right now, so the advice can name a
 * control that actually exists there — and point the right way.
 *
 * THE DIRECTION IS THE POINT, not just the name. "Sensitivity" in Simple is a 1..10 macro;
 * the Advanced sliders it drives are THRESHOLDS and run inversely — alphaFromSensitivity maps
 * 1 -> 1.50 and 10 -> 0.10, deltaFromSensitivity 1 -> 0.40 and 10 -> 0.025. So "Turn
 * Sensitivity down", read by someone in Advanced looking at the slider whose friendlyLabel is
 * literally "Sensitivity" (it is beatAlpha's), tells them to drag it the way that makes the
 * firing WORSE. Naming the firmware label and the direction removes both halves of that.
 *
 * `key` follows the threshold shape, exactly as the Simple macro does: showing advice about
 * the parameter the device is not using is its own kind of wrong.
 */
export type SensitivityControl = {
  mode: "simple" | "advanced";
  key: "beatAlpha" | "beatSfDelta";
};

/* Simple naming, matching the wizard's own notes ("try turning Sensitivity up a step by
 * hand"). The wizard embeds this panel and has no Simple/Advanced switch of its own. */
const DEFAULT_SENSITIVITY_CONTROL: SensitivityControl = {
  mode: "simple",
  key: "beatAlpha",
};

function fireLessAdvice(c: SensitivityControl): string {
  if (c.mode === "simple") return "Turn Sensitivity down.";
  return `Raise ${AUDIO_PARAMS[c.key].firmwareLabel}, or raise ${AUDIO_PARAMS.beatFluxFloor.firmwareLabel} — both make it fire less. They are thresholds, so they run the opposite way to the Simple Sensitivity slider.`;
}

function fireMoreAdvice(c: SensitivityControl): string {
  if (c.mode === "simple") return "Turn Sensitivity up.";
  return `Lower ${AUDIO_PARAMS[c.key].firmwareLabel} — it is a threshold, so it runs the opposite way to the Simple Sensitivity slider.`;
}

export function computeVerdict(
  s: TelemetrySummary,
  sensitivity: SensitivityControl = DEFAULT_SENSITIVITY_CONTROL,
): Verdict {
  /* ORDER MATTERS HERE, and the discriminator is ageMs, not the frame count.
   *
   * When a stream dies at a venue the 10 s window drains to zero frames, so a bare
   * `frames === 0` check first demoted the failure to the neutral "Not listening yet" —
   * reporting a fresh start at exactly the moment the user needs to be told something broke.
   * But `!live` first is equally wrong: a screen that has just opened is also not live.
   *
   * `ageMs` separates them without any new state. summarizeTelemetry only returns Infinity
   * when the ring has never held a frame, and otherwise reports the true age of the newest
   * one — which survives the window draining. Infinite age means "never started"; a finite
   * age past the stale threshold means "was streaming, stopped". */
  if (!Number.isFinite(s.ageMs)) {
    return {
      kind: "no-data",
      tone: "neutral",
      title: "Not listening yet",
      detail: "Waiting for the glasses to send what they are hearing.",
    };
  }

  if (!s.live) {
    return {
      kind: "stale",
      tone: "warning",
      title: "No signal",
      detail:
        "The glasses stopped sending. These numbers are the last ones received.",
    };
  }

  /* Live, but nothing in the window — not reachable via summarizeTelemetry (live implies a
   * frame inside the stale threshold, which is far shorter than the window), so this is a
   * guard against a future caller synthesising a summary rather than an observed state. */
  if (s.frames === 0) {
    return {
      kind: "no-data",
      tone: "neutral",
      title: "Not listening yet",
      detail: "Waiting for the glasses to send what they are hearing.",
    };
  }

  if (s.silentFraction > MUTED_FRACTION) {
    return {
      kind: "muted",
      tone: "bad",
      title: `Muted ${pct(s.silentFraction)}% of the time`,
      detail:
        "The glasses think the room is quiet, so the lights stop reacting. Turn down Ignore background noise.",
    };
  }

  if (s.clipFraction > CLIP_FRACTION) {
    return {
      kind: "clipping",
      tone: "warning",
      title: "The mic is overloading",
      detail:
        "It backs off on its own, but you can lower Loudest we want to help it along.",
    };
  }

  if (s.gainPinnedHigh) {
    return {
      kind: "gain-pinned",
      tone: "warning",
      title: "Mic is at full gain",
      detail:
        "The music is quiet where the glasses are. Move closer to a speaker before changing anything here.",
    };
  }

  /* Same MIN_FRAMES guard as the insensitive arm below. Without it, the first few ticks
   * after arming have a tiny span and sticky-OR'd beat flags, so beats-per-second spikes and
   * the banner flashed "Turn Sensitivity down" at a correctly-tuned device before settling.
   * A verdict that is wrong for the first second of every session teaches people to ignore
   * the banner. */
  if (
    s.frames >= MIN_FRAMES_FOR_SENSITIVITY &&
    s.beatsPerSecond > TOO_SENSITIVE_BPS
  ) {
    return {
      kind: "too-sensitive",
      tone: "warning",
      title: "Too sensitive",
      detail:
        `It is firing on almost every sound, including the snare. ${fireLessAdvice(sensitivity)}`,
    };
  }

  if (
    s.frames >= MIN_FRAMES_FOR_SENSITIVITY &&
    s.beatsPerSecond < INSENSITIVE_BPS
  ) {
    return {
      kind: "insensitive",
      tone: "warning",
      title: "Not finding beats",
      detail:
        `The glasses can hear the room but nothing is firing. ${fireMoreAdvice(sensitivity)}`,
    };
  }

  return {
    kind: "good",
    tone: "good",
    title: "Looking good",
    detail: s.bpm ? `Steady beats at about ${s.bpm} BPM.` : "Steady beats.",
  };
}
