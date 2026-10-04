/**
 * Slow-speed audio (§14): when the core produces audio slower than real
 * time (the emulator running below full speed), the ring holds a burst of
 * samples every so often and nothing in between. Played as is, that is
 * short bursts with hard edges - clicks and stutter. This stretches what
 * the core produced over the time it covers instead: a granular
 * overlap-add time stretch (Hann-windowed grains, 50% overlap, so
 * constant gain and no edges), each grain's source position advancing by
 * the measured production rate. Pitch is kept; the sound is continuous,
 * a little smeared. Pure (no audio-thread globals), for unit tests; the
 * worklet (ring-processor.ts) switches to it below SLOW_ENTER speed.
 */

export const GRAIN = 2048;
export const HOP = GRAIN / 2;
const CHANNELS = 2;
/** Speed (frames produced per frame played) below which stretching starts / above which it stops. */
export const SLOW_ENTER = 0.9;
export const SLOW_LEAVE = 0.97;
/** Below this the core is not producing audio at all (nothing to stretch). */
export const SLOW_FLOOR = 0.02;
/** The production-rate estimate's smoothing per 128-frame block (~0.5 s time constant). */
const RATE_ALPHA = 0.005;
const MIN_SPEED = 0.03;
/** Output frames without any new input before fading out (the core paused or loading). */
const STALL_FRAMES = 14400; // 300 ms at 48 kHz
/** Gain change per hop while fading in/out. */
const FADE_STEP = 0.25;

const WINDOW = Float32Array.from({ length: GRAIN }, (_, n) => 0.5 - 0.5 * Math.cos((2 * Math.PI * n) / GRAIN));

/** Where the ring is: the worklet's shared-memory views, or a test's arrays. */
export interface RingAccess {
  readonly frames: Float32Array; // interleaved stereo, capacity frames
  readonly capacity: number;
  writeIndex(): number;
  readIndex(): number;
  setReadIndex(index: number): void;
}

/** Tracks the production rate: frames the core wrote per frame played. */
export class RateMeter {
  rate = 1;
  private lastWrite: number | null = null;

  /** One output block of `played` frames, with the ring's write index now. */
  sample(write: number, played: number): number {
    if (this.lastWrite !== null) {
      const produced = (write - this.lastWrite) >>> 0;
      this.rate += RATE_ALPHA * (produced / played - this.rate);
    }
    this.lastWrite = write;
    return this.rate;
  }
}

export class SlowStretch {
  private readonly acc = new Float32Array(GRAIN * CHANNELS);
  private pos = HOP; // output frames used from the current hop; HOP = need a new grain
  private source = 0; // next grain's start (ring frame index)
  private started = false;
  private gain = 0;
  private sinceInput = 0;
  private lastWrite = 0;

  /** Fills `left`/`right` from the ring at `speed` (0..1]. */
  render(ring: RingAccess, left: Float32Array, right: Float32Array, speed: number): void {
    for (let i = 0; i < left.length; i++) {
      if (this.pos >= HOP) this.nextGrain(ring, Math.max(MIN_SPEED, Math.min(1, speed)));
      left[i] = this.acc[this.pos * CHANNELS] ?? 0;
      right[i] = this.acc[this.pos * CHANNELS + 1] ?? 0;
      this.pos++;
    }
  }

  private nextGrain(ring: RingAccess, speed: number): void {
    /* Shift out the hop just played. */
    this.acc.copyWithin(0, HOP * CHANNELS);
    this.acc.fill(0, HOP * CHANNELS);
    this.pos = 0;
    const write = ring.writeIndex() >>> 0;
    const read = ring.readIndex() >>> 0;
    if (write !== this.lastWrite) this.sinceInput = 0;
    else this.sinceInput += HOP;
    this.lastWrite = write;
    const fill = (write - read) >>> 0;
    if (!this.started) {
      if (fill < GRAIN) return; // wait for a whole grain
      this.started = true;
      this.source = read;
    }
    /* A grain needs GRAIN frames from `source`, still in the ring (>= read).
     * Short of that, take the newest GRAIN frames; too far behind (the
     * core outran us), skip ahead so latency stays about two grains. */
    let start = this.source;
    const ahead = (write - start) | 0;
    if (ahead < GRAIN) start = (write - GRAIN) >>> 0;
    else if (ahead > 2 * GRAIN) start = (write - 2 * GRAIN) >>> 0;
    if (((start - read) | 0) < 0) start = read;
    if (((write - start) | 0) < GRAIN) return; // not enough in the ring yet
    /* Fade out when the core stopped producing; back in when it resumes. */
    const target = this.sinceInput > STALL_FRAMES ? 0 : 1;
    this.gain = target > this.gain ? Math.min(target, this.gain + FADE_STEP) : Math.max(target, this.gain - FADE_STEP);
    if (this.gain > 0) {
      for (let n = 0; n < GRAIN; n++) {
        const at = ((start + n) % ring.capacity) * CHANNELS;
        const w = (WINDOW[n] ?? 0) * this.gain;
        this.acc[n * CHANNELS] = (this.acc[n * CHANNELS] ?? 0) + (ring.frames[at] ?? 0) * w;
        this.acc[n * CHANNELS + 1] = (this.acc[n * CHANNELS + 1] ?? 0) + (ring.frames[at + 1] ?? 0) * w;
      }
    }
    ring.setReadIndex(start);
    this.source = (start + Math.max(1, Math.round(HOP * speed))) >>> 0;
  }

  /** Back to normal playback: the next slow period starts fresh. */
  reset(): void {
    this.acc.fill(0);
    this.pos = HOP;
    this.started = false;
    this.gain = 0;
  }
}
