/**
 * The §14 audio drainer: an AudioWorkletProcessor reading the core's
 * audio ring straight out of the shared WebAssembly memory on the
 * real-time audio thread. Never waits; outputs silence on underrun (and
 * counts it); resamples by the §14 PI-controlled ratio (ring-control.ts)
 * with linear interpolation.
 *
 * Ring (core/audio/audio_ring.h): +0 write index, +4 read index (u32
 * frames), +8 float32 stereo frames.
 */
import { nextRatio, type RateState } from "./ring-control";

/* The AudioWorkletGlobalScope surface this module uses (not in lib.dom). */
declare abstract class AudioWorkletProcessor {
  readonly port: MessagePort;
  constructor(options?: unknown);
  abstract process(inputs: Float32Array[][], outputs: Float32Array[][], parameters: Record<string, Float32Array>): boolean;
}
declare function registerProcessor(name: string, ctor: new (options: { processorOptions: RingOptions }) => AudioWorkletProcessor): void;

export interface RingOptions {
  readonly memory: WebAssembly.Memory;
  readonly ringBase: number;
  readonly capacityFrames: number;
}

const CHANNELS = 2;
const HEADER_WORDS = 2;
const REPORT_EVERY_BLOCKS = 94; // ~250ms of 128-frame blocks at 48kHz

class RingProcessor extends AudioWorkletProcessor {
  private readonly indices: Int32Array;
  private readonly frames: Float32Array;
  private readonly capacity: number;
  private rate: RateState = { integral: 0 };
  private phase = 0; // fractional position past the read index
  private underruns = 0;
  private blocks = 0;

  constructor(options: { processorOptions: RingOptions }) {
    super(options);
    const { memory, ringBase, capacityFrames } = options.processorOptions;
    this.capacity = capacityFrames;
    this.indices = new Int32Array(memory.buffer, ringBase, HEADER_WORDS);
    this.frames = new Float32Array(memory.buffer, ringBase + HEADER_WORDS * 4, capacityFrames * CHANNELS);
  }

  process(_inputs: Float32Array[][], outputs: Float32Array[][]): boolean {
    const out = outputs[0];
    const left = out?.[0];
    const right = out?.[1] ?? left;
    if (!left || !right) return true;
    const write = Atomics.load(this.indices, 0) >>> 0;
    let read = Atomics.load(this.indices, 1) >>> 0;
    const fill = (write - read) >>> 0;
    const step = nextRatio(this.rate, fill, this.capacity);
    this.rate = step.state;
    let available = fill;
    for (let i = 0; i < left.length; i++) {
      if (available < 2) { // need this frame and the next to interpolate
        left[i] = 0;
        right[i] = 0;
        this.underruns++;
        continue;
      }
      const a = (read % this.capacity) * CHANNELS;
      const b = ((read + 1) % this.capacity) * CHANNELS;
      const t = this.phase;
      left[i] = (this.frames[a] ?? 0) * (1 - t) + (this.frames[b] ?? 0) * t;
      right[i] = (this.frames[a + 1] ?? 0) * (1 - t) + (this.frames[b + 1] ?? 0) * t;
      this.phase += step.ratio;
      const whole = Math.floor(this.phase);
      this.phase -= whole;
      read = (read + whole) >>> 0;
      available -= whole;
    }
    Atomics.store(this.indices, 1, read | 0);
    if (++this.blocks % REPORT_EVERY_BLOCKS === 0) {
      this.port.postMessage({ blocks: this.blocks, underruns: this.underruns, fill, ratio: step.ratio });
    }
    return true;
  }
}

registerProcessor("voland-audio-ring", RingProcessor);
