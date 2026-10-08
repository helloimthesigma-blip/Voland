# GPU command stream (version 5)

The CPU worker's records for the GPU worker's WebGPU renderer (DESIGN.md
§13). Producer: `core/gpu/raster3d.c` in GPU mode (`raster3d_set_gpu`,
turned on by `emulator_set_gpu_mode`). Consumer: `platform/web/workers/
gpu.worker.ts` through `gpu-executor.ts`. C definitions:
`core/gpu/gpu_stream.h` (transport) and `core/gpu/gpu_records.h` (records);
TypeScript mirror: `platform/web/bindings/gpu-records.ts`. Any change bumps
`GPU_STREAM_VERSION` and this document.

## Division of labour

The CPU side does everything up to the rasteriser, exactly as the software
reference renderer does: index/vertex fetch, the vertex program (Maxwell
interpreter), primitive assembly, clipping, the viewport transform and face
culling. What crosses is screen-space triangles plus the pixel program
translated to WGSL (`core/gpu/wgsl.h`). The GPU rasterises, runs the pixel
program, and does depth/stencil, blending and the colour write mask.

Render targets exist only as GPU textures: their guest memory is not
written. The guest's presents of them become PRESENT records, and 2D-engine
blits from them become COPY records. Textures are decoded on the CPU
(block-linear, BCn, ASTC) and uploaded once, then again only when their
guest bytes change.

## Transport

A single-producer / single-consumer byte ring in linear memory. In the web
build both the header and the ring live in the layout's `gpu_ring` region:
the header is at its base and the ring starts at +64. In `voland-cli
--gpu-stream` the ring is a static buffer.

| Offset | Field |
|---|---|
| +0 | u32 magic `VGPU` (0x55504756) |
| +4 | u32 version (4) |
| +8 | u64 ring base (linear-memory offset) |
| +16 | u64 ring capacity |
| +24 | u64 write position (monotonic bytes; release-stored on publish) |
| +32 | u64 read position (monotonic; advanced by the consumer) |
| +40 | i32 write signal (bumped + `memory.atomic.notify` on publish) |
| +44 | i32 read signal (bumped + `Atomics.notify` by the consumer) |
| +48 | u32 presents (PRESENT records written; the frame-rate meter reads it) |

- **Records.** Each record is `u32 type, u32 size` (header included, a
  multiple of 8), then the payload. A record never straddles the ring's
  end: the producer writes a PAD record (type 0) to the end and continues
  at offset 0.
- **Producer.** Publishes at the end of every submission and at every
  present. When the ring is full it waits on the read signal
  (`memory.atomic.wait32`, 20 ms slices).
- **Consumer.** Waits on the write signal with `Atomics.waitAsync`. It
  executes records in place and advances the read position after each one.
- **Size limit.** A record is at most half the ring (`gpu_stream_max_payload`).
  Large uploads are split into row chunks; a draw with more vertices than
  fit is split into several DRAW records.

## Records

All fields are u32 little-endian unless noted. Ids are the producer's; 0
means none.

| Type | Name | Payload |
|---|---|---|
| 1 | TEXTURE_CREATE | id, format (`GPU_FMT_*`), width, height, layers, usage (1 sampled, 2 render target), mip levels, reserved |
| 2 | TEXTURE_DESTROY | id |
| 3 | TEXTURE_WRITE | id, x, y, width, height, layer, bytes per row, data bytes; then the rows |
| 4 | SHADER | id, byte count, WGSL text (padded) |
| 5 | CLEAR | colour id, colour write mask, colour[4] (f32 bits or integers), depth id, flags (1 colour, 2 depth, 4 stencil), depth (f32), stencil, stencil mask, rect x, y, w, h |
| 6 | DRAW | `Gpu_Rec_Draw`, then its bindings, then the vertices |
| 7 | COPY | source id, destination id, source rect, destination rect, filter |
| 8 | PRESENT | id, rect, flags (1 flip x, 2 flip y) |

### DRAW

`Gpu_Rec_Draw` contains:

- **Shader:** the shader id.
- **Colour targets:** up to 8, each with its id, write mask, blend enable,
  and the colour and alpha op, source and destination factors. Factors,
  ops and compare functions are indices into WebGPU's enums.
- **Depth/stencil:** the depth target id, depth test, write and compare,
  stencil enable, front and back stencil faces, and the read mask, write
  mask and reference.
- **Fixed-function extras:** the blend constant and the scissor rectangle.
- **Counts:** varying count, flat mask, binding count and vertex count.
- **Vertex stage (version 3):** the vertex shader id (0: none), the vertex
  input count, the cull mode (0 none, 1 front, 2 back) and the front face
  (0 counter-clockwise, 1 clockwise).

Bindings follow the header:

- **DATA** (kind 1, binding 0): its bytes follow the binding entry. They
  are the draw constants of `wgsl.h` (surface height, flags, alpha test,
  per-texture sampler state, the constant-buffer table), followed by the
  constant buffers the program reads.
- **TEXTURE** (kind 2, binding 1 + i): texture id. The pixel program reads
  it with `textureLoad` and does wrap, border, filtering, depth compare
  and swizzle itself, from the draw constants.
- **SAMPLER** (kind 3, binding 17 + i): a hardware sampler for texture i
  where that gives the reference's result; its state word is bit 0
  magnification linear, 2 bits per axis u, v, w (0 repeat, 1 mirror, 2
  clamp to edge) from bit 1, bit 7 minification linear and bit 8 linear
  between mip levels.

### Mip levels (version 2)

A sampled texture whose guest header has a mip chain is created with
`levels` > 1 (float 2D and 2D-array textures only). The producer uploads
level 0. The consumer rebuilds the other levels from it with a 2x2 box
filter, before the next draw that samples a texture written since. Pixel
programs choose the level per pixel: from derivatives of the texel-space
coordinates in uniform control flow, from the instruction's explicit LOD
(LL), or level 0 (LZ, and wherever derivatives are undefined). They then
apply the sampler's bias and its min/max LOD clamps (draw constants,
`wgsl.h`). A one-level texture samples exactly as in version 1.
`VOLAND_GPU_MIPMAPS=0 voland-cli ...` records level 0 only, which matches
the software reference.

Version 1 streams still replay: a 24-byte TEXTURE_CREATE has one level.

Vertices form a triangle list. Each vertex is x, y in WebGPU NDC, z, 1/w
(f32), then `varying_count` × 4 u32. Varyings are already divided by w
where the program interpolates them perspective-correct, so the GPU
interpolates screen-linearly; flat ones are the provoking vertex's.
Front-facing triangles wind counter-clockwise in NDC. Culling is already
done, so the pipeline uses `cullMode: none`, and the winding only feeds
`@builtin(front_facing)`.

### Vertex stage (version 3)

When `vs_shader_id` is not 0, the vertices are not transformed. Each one is
`1 + vertex_input_count` × vec4<u32>: (vertex id, instance id, 0, 0), then
the vertex program's input vectors as fetched (locations 0..count). They
go through the `vs` entry point of shader `vs_shader_id`. That entry
point is a translated Maxwell vertex program whose epilogue applies the
viewport (the DATA binding's viewport words and its own constant-buffer
table, `wgsl.h`) and emits the pixel program's varyings. The provoking
vertex comes first in each triangle. The pipeline culls with `cull_mode`
and `front_face`. WebGPU judges winding in framebuffer coordinates (y
down), so `front_face` is the guest's winding on screen.

### Indexed vertex-stage draws (version 4)

`Gpu_Rec_Draw.index_count` (then a flags word, version 5) follows `front_face`.
When a vertex-stage draw has `index_count` > 0, its `vertex_count`
vertices are the draw's distinct vertices: each guest vertex once per
record, in first-use order. They are followed by `index_count` u32
indices, a triangle list with each triangle's provoking vertex first,
and then the record's padding. The consumer stages the indices after the
vertices in the same buffer and calls `drawIndexed`. Silksong's 250k-slice
stream shrinks from 197 to 128 MB, and an SSBU fight's vertex data was
about 27 MB per frame before this.

### Vertex pulling (version 5)

With `GPU_DRAW_VERTEX_PULL` in `Gpu_Rec_Draw.flags` a vertex-stage draw
has no vertices (`vertex_count` 0). Its `index_count` u32 indices are
guest vertex ids, the vertex stage's `@builtin(vertex_index)`, and the
pipeline has no vertex buffers. The vertex program decodes its inputs
itself (`vfetch` in `core/gpu/wgsl.c`). It reads them from the draw's
data binding, which carries after the constants and constant buffers:

- per input location l, four words at `WGSL_DRAW_VS_INPUTS + 4 l`
  (`core/gpu/wgsl.h`):
  - the byte offset in the binding of its stream's first copied element;
  - the stride | `WGSL_VSI_ACTIVE` | `WGSL_VSI_INSTANCED`;
  - the guest's VERTEX_ATTRIB word (offset, size, number type, BGRA swap);
  - the vertex id of the first copied element;
- the instance id at `WGSL_DRAW_VS_INSTANCE`;
- each stream's bytes over the record's id span (an instanced stream:
  the draw's one element), 8-byte aligned.

A record ends where its ids' span would no longer fit the 2 MiB data
window. A triangle whose own ids are too far apart switches the rest of
the draw to the version-4 form, with decoded inputs from the CPU.

The CPU's work per vertex goes away: no attribute fetch, no format
decoding and no per-record vertex map. In an SSBU fight, `fetch_attribute`
and its vertex window refills were about a third of the CPU worker before.
`voland-cli --no-vertex-pull` keeps the version-4 form.

## Verification

- `tests/wgsl_test.c` writes differential vectors (the interpreter is the
  oracle); `platform/web/tools/wgsl-vectors.mjs` renders them on WebGPU.
- It also writes `vertex_pull.json`: every attribute format, number type
  and BGRA swap at unaligned offsets, with raster3d's CPU decoder
  (`raster3d_decode_attribute`) as the oracle.
  `platform/web/tools/vertex-pull-vectors.mjs` runs the WGSL decoder in a
  compute shader on WebGPU and compares (254 cases on Metal).
- `voland-cli run X --gpu-stream F` records a stream. `node
  platform/web/tools/replay-gpu-stream.mjs F OUT [--adapter default]`
  replays it through the executor and writes the presented frames.
  Compare them with `voland-cli --dump-frame` from a software run of the
  same slice count: the runs are deterministic.
- `platform/web/tools/validate-wgsl.mjs DIR` compiles a corpus of
  translated programs (`VOLAND_DUMP_WGSL=DIR voland-cli run ...`).
