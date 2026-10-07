/**
 * Maxwell compute launches (class B1C0, DESIGN.md §13).
 *
 * A compute dispatch is a Queue Meta Data block (QMD, 256 bytes in GPU
 * memory) named by SEND_PCAS_A (its address >> 8) and started by
 * SEND_SIGNALING_PCAS_B. The QMD carries the program's offset into the
 * program region, the grid (CTA raster) and block (CTA thread dimension)
 * sizes, the shared memory size and up to eight constant buffers. Field
 * positions follow NVIDIA's published QMD v01_07 layout (open-gpu-doc,
 * clb1c0qmd.h); this is our own decoder.
 *
 * The program has no shader program header: its code starts at the
 * offset. It runs on the reference renderer's Maxwell interpreter, one
 * block (CTA) at a time, 32-lane groups interleaved at BAR.SYNC.
 */
#ifndef SWITCH_GPU_COMPUTE_H
#define SWITCH_GPU_COMPUTE_H

#include <stdbool.h>
#include <stdint.h>

#define COMPUTE_QMD_BYTES 256u
#define COMPUTE_QMD_WORDS (COMPUTE_QMD_BYTES / 4u)
#define COMPUTE_QMD_ADDRESS_SHIFT 8u /* SEND_PCAS_A holds the QMD address >> 8 */
#define COMPUTE_CBUFS 8u
#define COMPUTE_MAX_BLOCK_THREADS 1024u
#define COMPUTE_MAX_SHARED_BYTES (48u * 1024u)

/* Class B1C0 methods (word index = byte offset / 4). */
#define COMPUTE_REGISTER_WORDS 0x1000u
#define COMPUTE_METHOD_SEND_PCAS_A (0x02b4u / 4u)
#define COMPUTE_METHOD_SEND_SIGNALING_PCAS_B (0x02bcu / 4u)
#define COMPUTE_PCAS_B_SCHEDULE 2u /* bit 1: schedule the QMD */
#define COMPUTE_METHOD_TEX_SAMPLER_POOL (0x155cu / 4u) /* A (upper), B (lower), C (max index) */
#define COMPUTE_METHOD_TEX_HEADER_POOL (0x1574u / 4u)
#define COMPUTE_METHOD_PROGRAM_REGION (0x1608u / 4u)   /* A (upper), B (lower) */
#define COMPUTE_METHOD_BINDLESS_TEXTURE (0x2608u / 4u) /* constant buffer slot of texture handles */

typedef struct Compute_Launch {
  uint32_t program_offset;          /* from the program region */
  uint32_t grid[3];                 /* CTA raster width, height, depth */
  uint32_t block[3];                /* CTA thread dimensions */
  uint32_t shared_bytes;
  uint32_t local_bytes;             /* per-thread local memory (low) */
  uint32_t barrier_count;
  uint32_t register_count;
  uint32_t cbuf_valid;              /* bit per slot */
  uint64_t cbuf_address[COMPUTE_CBUFS];
  uint32_t cbuf_size[COMPUTE_CBUFS];
} Compute_Launch;

/* Decodes a QMD. False when it describes nothing runnable (an empty grid
 * or block, or a block over COMPUTE_MAX_BLOCK_THREADS). */
bool compute_qmd_parse(const uint32_t qmd[COMPUTE_QMD_WORDS], Compute_Launch *out);

#endif /* SWITCH_GPU_COMPUTE_H */
