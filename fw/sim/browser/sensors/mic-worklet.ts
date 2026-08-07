/**
 * AudioWorklet processor that turns live microphone audio into the same
 * 512-sample 16 kHz mono int16 blocks the firmware DSP consumes.
 *
 * It is deliberately self-contained (no imports): an AudioWorklet module is
 * loaded into its own global scope by addModule(), and the block size /
 * sample rate arrive via processorOptions rather than from core/providers,
 * so this file stays loadable as a standalone module.
 *
 * When the AudioContext could not be opened at the target rate (Firefox and
 * Safari may ignore the requested rate) the input is linearly resampled here
 * rather than on the main thread — the render quantum is only 128 frames, so
 * doing it at the source avoids buffering the full-rate stream.
 */

/* Ambient declarations for the AudioWorklet global scope, which is not part
 * of lib.dom. Kept module-local so they don't leak into the UI's types. */
declare const sampleRate: number;
declare class AudioWorkletProcessor {
  readonly port: MessagePort;
  constructor(options?: unknown);
}
declare function registerProcessor(
  name: string,
  ctor: new (options?: unknown) => AudioWorkletProcessor & {
    process(
      inputs: Float32Array[][],
      outputs: Float32Array[][],
      params: Record<string, Float32Array>,
    ): boolean;
  },
): void;

interface MicOptions {
  processorOptions?: { targetRate?: number; blockSize?: number };
}

class MicCaptureProcessor extends AudioWorkletProcessor {
  private readonly blockSize: number;
  /** Input samples consumed per output sample; 1.0 when no resampling. */
  private readonly step: number;
  private block: Int16Array;
  private filled = 0;
  /** Read position within the CURRENT render quantum, carried across
   * quanta by subtracting the quantum length — so no seam bookkeeping is
   * needed beyond clamping the interpolation partner at the last sample. */
  private readPos = 0;

  constructor(options?: unknown) {
    super(options);
    const opts = (options as MicOptions | undefined)?.processorOptions ?? {};
    this.blockSize = opts.blockSize ?? 512;
    this.step = sampleRate / (opts.targetRate ?? 16000);
    this.block = new Int16Array(this.blockSize);
  }

  process(inputs: Float32Array[][]): boolean {
    const channel = inputs[0]?.[0];
    if (channel === undefined || channel.length === 0) {
      // No input connected yet — stay alive, the graph may connect later.
      return true;
    }

    const n = channel.length;
    while (this.readPos < n) {
      const i = Math.floor(this.readPos);
      const frac = this.readPos - i;
      const s0 = channel[i];
      const s1 = channel[i + 1 < n ? i + 1 : n - 1];
      this.block[this.filled++] = clampToInt16(s0 + (s1 - s0) * frac);
      if (this.filled === this.blockSize) {
        // postMessage transfers the buffer, so hand over a fresh one.
        const full = this.block;
        this.block = new Int16Array(this.blockSize);
        this.filled = 0;
        this.port.postMessage(full, [full.buffer]);
      }
      this.readPos += this.step;
    }
    this.readPos -= n;
    return true;
  }
}

function clampToInt16(v: number): number {
  const scaled = Math.round(v * 32767);
  return scaled > 32767 ? 32767 : scaled < -32768 ? -32768 : scaled;
}

registerProcessor("rgbx-mic-capture", MicCaptureProcessor);

export {};
