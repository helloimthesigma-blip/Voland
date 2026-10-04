/**
 * Unit tests for src/audio/slow-stretch.ts: below-real-time audio is
 * stretched into continuous, unit-gain, edge-free output; a stalled core
 * fades to silence; the rate meter follows the production rate.
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { GRAIN, RateMeter, type RingAccess, SlowStretch } from "../../src/audio/slow-stretch.ts";

const CAPACITY = 4096;
const BLOCK = 128;

function makeRing(): { ring: RingAccess; write: (frames: number, value: (i: number) => number) => void } {
  const frames = new Float32Array(CAPACITY * 2);
  let w = 0;
  let r = 0;
  const ring: RingAccess = {
    frames,
    capacity: CAPACITY,
    writeIndex: () => w,
    readIndex: () => r,
    setReadIndex: (i) => { r = i >>> 0; },
  };
  const write = (n: number, value: (i: number) => number): void => {
    const space = CAPACITY - ((w - r) >>> 0);
    for (let k = 0; k < Math.min(n, space); k++) {
      frames[((w + k) % CAPACITY) * 2] = value(w + k);
      frames[((w + k) % CAPACITY) * 2 + 1] = value(w + k);
    }
    w = (w + Math.min(n, space)) >>> 0;
  };
  return { ring, write };
}

test("a steady signal produced at 25% speed comes out continuous at unit gain", () => {
  const { ring, write } = makeRing();
  const stretch = new SlowStretch();
  const left = new Float32Array(BLOCK);
  const right = new Float32Array(BLOCK);
  const out: number[] = [];
  let produced = 0;
  for (let t = 0; t < 400; t++) {
    /* 1200-frame bursts at a quarter of the playback rate. */
    while (produced + 1200 <= (t + 1) * BLOCK * 0.25) {
      write(1200, () => 0.5);
      produced += 1200;
    }
    stretch.render(ring, left, right, 0.25);
    out.push(...left);
  }
  /* After the start (a grain to fill, a fade-in), no gaps and the value holds. */
  const settled = out.slice(10 * GRAIN);
  assert.ok(settled.length > 10_000);
  for (const v of settled) assert.ok(Math.abs(v - 0.5) < 1e-3, `sample ${v} should be 0.5`);
});

test("a stalled core fades out instead of looping the last grain", () => {
  const { ring, write } = makeRing();
  const stretch = new SlowStretch();
  const left = new Float32Array(BLOCK);
  const right = new Float32Array(BLOCK);
  write(4000, () => 0.5);
  let last = 0;
  for (let t = 0; t < 300; t++) { /* ~0.8 s with no new input */
    stretch.render(ring, left, right, 0.25);
    last = left[BLOCK - 1] ?? 0;
  }
  assert.equal(last, 0);
});

test("the rate meter follows production", () => {
  const meter = new RateMeter();
  let write = 0;
  let rate = 1;
  for (let t = 0; t < 4000; t++) {
    write += 32; /* 32 frames per 128 played: 25% */
    rate = meter.sample(write, BLOCK);
  }
  assert.ok(Math.abs(rate - 0.25) < 0.01, `rate ${rate}`);
});
