/**
 * Persistent shader/pipeline cache format (workers/shader-cache.ts).
 */
import assert from "node:assert/strict";
import { test } from "node:test";

import { type PipelineSpec, SHADER_CACHE_VERSION, parseCache, serializeCache, specKey, wgslHash } from "../../workers/shader-cache.ts";

const SPEC: PipelineSpec = {
  wgsl: wgslHash("@fragment fn fs() {}"),
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
