/**
 * GPU buffers mirroring guest GPU memory (GPU stream version 6): what
 * compute dispatches run on the GPU read and write (docs/GPU_COMMAND_STREAM.md
 * "Storage buffers").
 *
 * A mirror is a GPU storage buffer standing for [va, va + bytes) of guest
 * GPU virtual memory, in 4 KiB pages. Each page is owned by one side:
 *
 *   - CPU-owned (the default): guest memory is the truth. A sync, at most
 *     once per submission, hashes the page and uploads it when it changed.
 *   - GPU-owned: a dispatch wrote it (gpu_mirror_take). The mirror is the
 *     truth and guest memory is stale, since nothing is copied back. Draws
 *     that pull vertex streams from it bind the mirror instead
 *     (gpu_mirror_resident). If the guest's own copy of the page changes,
 *     the guest rewrote it, and the page is CPU-owned again.
 *
 * One renderer at a time owns the mirrors (its GPU thread).
 */
#ifndef SWITCH_GPU_GPU_MIRROR_H
#define SWITCH_GPU_GPU_MIRROR_H

#include <stdbool.h>
#include <stdint.h>

#include "gpu/gpu_stream.h"

struct Gpu_Memory;

#define GPU_MIRROR_PAGE_BYTES 4096u
#define GPU_MIRROR_MAX_BYTES (4u << 20)
#define GPU_MIRROR_COUNT 32u

typedef struct Gpu_Mirror_Stats {
  uint64_t created, destroyed;
  uint64_t pages_hashed, pages_uploaded;
  uint64_t pages_taken;    /* pages a dispatch made GPU-owned */
  uint64_t pages_returned; /* GPU-owned pages the guest rewrote */
} Gpu_Mirror_Stats;

/* Forgets every mirror (a new stream, or a reset); `stream` may be NULL. */
void gpu_mirror_reset(Gpu_Stream *stream);

/* The mirror holding [va, va + bytes), created (and the overlapped ones
 * replaced) as needed, synced if it was not synced in `submission`: its
 * buffer id, and in *offset where va lies in it. 0 when it cannot be
 * mirrored (over GPU_MIRROR_MAX_BYTES, or no stream). */
uint32_t gpu_mirror_window(Gpu_Stream *stream, const struct Gpu_Memory *mem, uint64_t va, uint32_t bytes,
                           uint32_t submission, uint32_t *offset);

/* A dispatch wrote [va, va + bytes) of a mirror: those pages become
 * GPU-owned (their guest copy is remembered by hash). */
void gpu_mirror_take(const struct Gpu_Memory *mem, uint64_t va, uint32_t bytes);

/* Whether [va, va + bytes) has GPU-owned pages, all inside one mirror:
 * readers must use the mirror (its id, and va's offset in it). */
bool gpu_mirror_resident(uint64_t va, uint64_t bytes, uint32_t *id, uint32_t *offset);

/* The CPU side wrote [va, va + bytes) itself (a compute dispatch run on
 * the CPU): the pages are CPU-owned and upload at the next sync. */
void gpu_mirror_cpu_wrote(uint64_t va, uint64_t bytes);

/* The size in bytes of mirror `id` (0: none). */
uint32_t gpu_mirror_bytes(uint32_t id);

const Gpu_Mirror_Stats *gpu_mirror_stats(void);

#endif /* SWITCH_GPU_GPU_MIRROR_H */
