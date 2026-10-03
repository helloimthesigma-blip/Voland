# GPU command stream (version 1)

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
| +4 | u32 version (1) |
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
| 1 | TEXTURE_CREATE | id, format (`GPU_FMT_*`), width, height, layers, usage (1 sampled, 2 render target) |
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

Bindings follow the header:

- **DATA** (kind 1, binding 0): its bytes follow the binding entry. They
  are the draw constants of `wgsl.h` (surface height, flags, alpha test,
  per-texture sampler state, the constant-buffer table), followed by the
  constant buffers the program reads.
- **TEXTURE** (kind 2, binding 1 + i): texture id. The pixel program reads
  it with `textureLoad` and does wrap, border, filtering, depth compare
  and swizzle itself, from the draw constants.

Vertices form a triangle list. Each vertex is x, y in WebGPU NDC, z, 1/w
(f32), then `varying_count` × 4 u32. Varyings are already divided by w
where the program interpolates them perspective-correct, so the GPU
interpolates screen-linearly; flat ones are the provoking vertex's.
Front-facing triangles wind counter-clockwise in NDC. Culling is already
done, so the pipeline uses `cullMode: none`, and the winding only feeds
`@builtin(front_facing)`.

## Verification

- `tests/wgsl_test.c` writes differential vectors (the interpreter is the
  oracle); `platform/web/tools/wgsl-vectors.mjs` renders them on WebGPU.
- `voland-cli run X --gpu-stream F` records a stream. `node
  platform/web/tools/replay-gpu-stream.mjs F OUT [--adapter default]`
  replays it through the executor and writes the presented frames.
  Compare them with `voland-cli --dump-frame` from a software run of the
  same slice count: the runs are deterministic.
- `platform/web/tools/validate-wgsl.mjs DIR` compiles a corpus of
  translated programs (`VOLAND_DUMP_WGSL=DIR voland-cli run ...`).
