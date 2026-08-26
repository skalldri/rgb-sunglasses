import { memo } from "react";
import { StyleSheet, View } from "react-native";

import { BandHeadroomBars } from "@/components/audio/band-headroom-bars";
import { BeatPulse } from "@/components/audio/beat-pulse";
import { InputLevelMeter } from "@/components/audio/input-level-meter";
import { SpectrumBars } from "@/components/audio/spectrum-bars";
import { VerdictBanner } from "@/components/audio/verdict-banner";
import { ThemedText } from "@/components/themed-text";
import { Radii, Spacing } from "@/constants/theme";
import {
  useAudioTelemetry,
  useAudioTelemetryStatus,
  useAudioTelemetrySummary,
} from "@/context/audio-telemetry-context";
import { useThemeColors } from "@/hooks/use-theme-color";
import {
  computeVerdict,
  type SensitivityControl,
} from "@/services/audio-scoreboard";
import {
  AUDIO_NUM_BANDS,
  BAND_RATIO_MAX,
  TELEMETRY_TIER_METERS,
  TELEMETRY_TIER_SPECTRUM,
  dequantiseLog,
  ringIndex,
} from "@/services/audio-telemetry";

/**
 * The sticky monitor: what the glasses are hearing, right now.
 *
 * Re-renders at most twice a second (the summary tick) regardless of the stream rate — the
 * meters inside it animate off shared values on the UI thread and are not part of that.
 *
 * HONEST FREEZING: when the stream stalls, everything dims and says NO SIGNAL rather than
 * decaying toward zero. A meter that drifts down on its own is lying about the room, and at a
 * venue that lie costs someone ten minutes of tuning against a dead stream.
 */

interface Props {
  targetLow: number | null;
  targetHigh: number | null;
  noiseGate: number | null;
  /**
   * How the surrounding screen presents sensitivity, so the verdict names a control that is
   * actually on screen and points the right way. Omitted by the calibration wizard, which has
   * no Simple/Advanced switch and whose own notes speak in the Simple macro's words.
   */
  sensitivity?: SensitivityControl;
  testID?: string;
}

export const MonitorPanel = memo(function MonitorPanel({
  targetLow,
  targetHigh,
  noiseGate,
  sensitivity,
  testID = "audio-monitor",
}: Props) {
  const colors = useThemeColors();
  const summary = useAudioTelemetrySummary();
  const status = useAudioTelemetryStatus();
  const telemetry = useAudioTelemetry();
  const verdict = computeVerdict(summary, sensitivity);

  /* Read the newest ratios straight off the ring for the accessibility labels. The bars
   * themselves do not use these — they animate off shared values — so this costs one pass
   * over four bytes per summary tick, not per frame. */
  const ratios: number[] = [];
  if (telemetry) {
    const ring = telemetry.ring.current;
    const i = ringIndex(ring, 0);
    if (i >= 0) {
      const base = i * AUDIO_NUM_BANDS;
      for (let b = 0; b < AUDIO_NUM_BANDS; b++) {
        const flux = dequantiseLog(ring.flux[base + b]);
        const threshold = dequantiseLog(ring.threshold[base + b]);
        /* Clamped with the SAME constant the provider clamps the bars with. Unclamped, the
         * bar pinned at full while this label announced "480 percent of the firing level" —
         * one tick meaning two different things to a sighted and a screen-reader user. */
        ratios.push(
          threshold > 0 ? Math.min(flux / threshold, BAND_RATIO_MAX) : 0,
        );
      }
    }
  }

  if (status === "unsupported") {
    return (
      <View
        testID={`${testID}-unsupported`}
        style={[
          styles.card,
          { backgroundColor: colors.surface, borderColor: colors.border },
        ]}
      >
        <ThemedText type="defaultSemiBold">
          Live meters need newer firmware
        </ThemedText>
        <ThemedText type="caption" style={{ color: colors.textSecondary }}>
          You can still tune everything below — this device just cannot show you
          what it is hearing yet.
        </ThemedText>
      </View>
    );
  }

  const live = summary.live;
  /* Three states, not two. Before any frame arrives the tier is OFF, which is NOT the same as
   * "this link cannot carry a spectrum" — claiming a degraded link while simply waiting for
   * the first frame sends someone off to re-pair a connection that is fine. Hardware-found
   * 2026-08-25: the panel accused the link of running at its smallest packet size while the
   * real problem was that no frame had been decoded at all. */
  const haveFrames = summary.frames > 0;
  const spectrumDowngraded =
    haveFrames && summary.tier < TELEMETRY_TIER_SPECTRUM;
  /* Only tier 1 is evidence of an MTU-limited link: it is the 20-byte tier that exists to
   * survive an unnegotiated ATT MTU of 23. Tier 2 is 28 bytes, which fits every MTU this
   * stack actually negotiates, so seeing it means something ASKED for a reduced tier — the
   * calibration wizard does exactly that for its tap-along step. Prescribing a re-pair there
   * sends the user to fix a link that is working perfectly. */
  const mtuLimited = summary.tier === TELEMETRY_TIER_METERS;

  return (
    <View
      testID={testID}
      style={[
        styles.card,
        { backgroundColor: colors.surface, borderColor: colors.border },
      ]}
    >
      <View style={styles.headerRow}>
        {/* BeatPulse must be the one that gives way. Its own inner text is `flex: 1`, so
         * without a width constraint here it sizes to the space it would LIKE and pushes the
         * pill past the card's right edge and off the screen — "LIVE" rendered as "LIV" with
         * the pill half off-device. minWidth:0 is the half that is easy to omit: a flex child
         * will not shrink below its content width without it, so `flex: 1` alone does not fix
         * this. */}
        <View style={styles.pulseWrap}>
          <BeatPulse
            bpm={summary.bpm}
            beatsPerSecond={summary.beatsPerSecond}
            lastBeatBand={summary.lastBeatBand}
            live={live}
          />
        </View>
        <View
          testID={`${testID}-pill`}
          style={[
            styles.pill,
            {
              backgroundColor: live ? colors.success : colors.surfaceAlt,
              borderColor: colors.border,
            },
          ]}
        >
          <ThemedText
            type="caption"
            style={{ color: live ? colors.onPrimary : colors.textMuted }}
          >
            {live ? "LIVE" : "NO SIGNAL"}
          </ThemedText>
        </View>
      </View>

      {spectrumDowngraded ? (
        <ThemedText
          type="caption"
          style={{ color: colors.textMuted }}
          testID={`${testID}-no-spectrum`}
        >
          {mtuLimited
            ? "Spectrum unavailable — the connection is running at its smallest packet size. Re-pairing usually fixes it."
            : "Spectrum paused while the glasses send more detail instead."}
        </ThemedText>
      ) : (
        <SpectrumBars live={live && haveFrames} />
      )}

      <InputLevelMeter
        targetLow={targetLow}
        targetHigh={targetHigh}
        noiseGate={noiseGate}
        rmsInputDb={summary.rmsInputDb}
        gainDb={summary.gainDb}
        headroomDb={summary.headroomDb}
        live={live}
      />

      <BandHeadroomBars ratios={ratios} live={live} />

      <VerdictBanner verdict={verdict} />
    </View>
  );
});

const styles = StyleSheet.create({
  card: {
    borderWidth: StyleSheet.hairlineWidth,
    borderRadius: Radii.lg,
    padding: Spacing.md,
    gap: Spacing.md,
  },
  headerRow: {
    flexDirection: "row",
    alignItems: "center",
    justifyContent: "space-between",
  },
  pulseWrap: { flex: 1, minWidth: 0 },
  pill: {
    paddingHorizontal: Spacing.sm,
    paddingVertical: 2,
    borderRadius: Radii.pill,
    borderWidth: StyleSheet.hairlineWidth,
    /* Never give up width: this is the status the whole panel is judged by, and "NO SIGNAL"
     * is the longer, more important of the two strings it can hold. */
    flexShrink: 0,
  },
});
