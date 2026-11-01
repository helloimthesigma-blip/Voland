/**
 * Persistent shader/pipeline cache format (workers/shader-cache.ts).
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { type PipelineSpec, SHADER_CACHE_VERSION, parseCache, roundBiasSlope, serializeCache, specKey, wgslHash } from "../../workers/shader-cache.ts";

const SPEC: PipelineSpec = {
  wgsl: wgslHash("@fragment fn fs() {}"),
  vs: null,
  varyings: 2,
  textures: ["F", "uint"],
  targets: [{ format: "rgba8unorm", writeMask: 15, blend: [0, 6, 7, 0, 1, 0] }, { format: null, writeMask: 0, blend: null }],
  depth: { format: "depth24plus-stencil8", test: true, write: false, compare: 3, stencil: null },
};

test("WGSL hashes are stable, 16 hex digits, and tell texts apart", () => {
  assert.equal(wgslHash("abc"), wgslHash("abc"));
  assert.match(wgslHash("abc"), /^[0-9a-f]{16}$/);
  assert.notEqual(wgslHash("abc"), wgslHash("abd"));
  assert.notEqual(wgslHash("ab"), wgslHash("ba"));
});

test("a cache round-trips shaders and pipelines", () => {
  const shaders = new Map([[SPEC.wgsl, "@fragment fn fs() {}"]]);
  const back = parseCache(serializeCache({ shaders, pipelines: [SPEC] }));
  assert.deepEqual([...back.shaders], [...shaders]);
  assert.equal(back.pipelines.length, 1);
  assert.equal(specKey(back.pipelines[0] as PipelineSpec), specKey(SPEC));
});

test("damaged, other-version and orphaned entries are dropped", () => {
  assert.equal(parseCache("{not json").shaders.size, 0);
  assert.equal(parseCache(JSON.stringify({ version: SHADER_CACHE_VERSION + 1, shaders: { a: "x" }, pipelines: [] })).shaders.size, 0);
  /* A pipeline whose shader is not stored cannot be built: dropped. */
  const orphan = parseCache(JSON.stringify({ version: SHADER_CACHE_VERSION, shaders: {}, pipelines: [SPEC] }));
  assert.equal(orphan.pipelines.length, 0);
});

test("slope-scaled depth bias keeps 4 significant bits, so drifting values share pipelines", () => {
  assert.equal(roundBiasSlope(0), 0);
  assert.equal(roundBiasSlope(-0.5), -0.5);
  assert.equal(roundBiasSlope(2), 2);
  /* TOTK's camera-driven slopes: the result stays within 1/32. */
  for (const v of [-0.211111113, -0.511111081, -0.40746069, -0.707460701, 1.37, 1e-5]) {
    assert.ok(Math.abs(roundBiasSlope(v) - v) <= Math.abs(v) / 32);
  }
  /* A sweep of 1000 values in one octave lands on at most 2^4 + 1 keys. */
  const keys = new Set<number>();
  for (let i = 0; i < 1000; i++) keys.add(roundBiasSlope(-(0.5 + i / 2000)));
  assert.ok(keys.size <= 17);
});
