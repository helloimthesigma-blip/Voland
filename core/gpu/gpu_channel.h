/**
 * GPU channel command processing (§13): what PFIFO does with a submitted
 * GPFIFO - fetch each entry's pushbuffer segment, decode method headers,
 * bind classes to subchannels, run host methods (semaphores, syncpoint
 * increments) and hand engine methods to the engine bound on that
 * subchannel.
 *
 * Engines implemented here, CPU-side, because their work is memory
 * movement rather than rendering:
 *   - Maxwell DMA (class B0B5, "copy engine"): 1D and multi-line copies
 *     between pitch-linear and block-linear surfaces, with component
 *     remapping and semaphore release - what deko3d/NVN use for buffer <->
 *     image copies (homebrew menus present this way).
 *   - Maxwell 3D (class B197), synchronization only: the register file is
 *     kept, and SYNCPT_ACTION increments and REPORT_SEMAPHORE releases
 *     execute - deko3d/NVN signal their fences this way.
 * Rendering methods (3D draws, compute, 2D, inline-to-memory) are kept or
 * counted and otherwise ignored until their engines land in the GPU
 * worker (§13 command ring).
 *
 * Wire formats (NVIDIA's published host/class headers, e.g. open-gpu-doc
 * clb06f.h / clb0b5.h):
 *   GP entry (u64): bits 2-39 segment GPU VA, bits 42-62 length in words
 *     (0 = control entry, no fetch).
 *   Method header (u32): bits 0-12 method (word address), 13-15
 *     subchannel, 16-28 count (or immediate data), 29-31 operation:
 *     1 incrementing, 3 non-incrementing, 4 immediate, 5 increment-once.
 *   Host methods (word addresses < 0x40): 0x00 bind object (class),
 *     0x04-0x07 semaphore address hi/lo, payload, execute (op 1 acquire,
 *     2 release; bit 24 = 4-byte release), 0x1C/0x1D syncpoint payload /
 *     operation (bit 0 increment, bits 8-23 id).
 */
#ifndef SWITCH_GPU_GPU_CHANNEL_H
#define SWITCH_GPU_GPU_CHANNEL_H

#include <stdbool.h>
#include <stdint.h>

#define GPU_SUBCHANNELS 8u
#define GPU_DMA_REGISTER_WORDS 0x200u /* B0B5 method space through 0x7FC */
#define GPU_3D_REGISTER_WORDS 0xE00u  /* B197 method space */
#define GPU_LINE_BYTES 0x10000u       /* longest DMA line handled */
#define GPU_FETCH_WORDS 0x1000u       /* pushbuffer words fetched at a time */

#define GPU_CLASS_DMA 0xB0B5u
#define GPU_CLASS_3D 0xB197u
#define GPU_CLASS_COMPUTE 0xB1C0u
#define GPU_CLASS_2D 0x902Du
#define GPU_CLASS_I2M 0xA140u

/* GPU virtual memory as the channel sees it (nvdrv's address space).
 * Both return false on an unmapped range. */
typedef struct Gpu_Memory {
  void *user;
  bool (*read)(void *user, uint64_t gpu_va, void *out, uint64_t size);
  bool (*write)(void *user, uint64_t gpu_va, const void *src, uint64_t size);
  /* An in-stream syncpoint increment (host method 0x1D). May be NULL. */
  void (*syncpoint_increment)(void *user, uint32_t id);
} Gpu_Memory;

typedef struct Gpu_Channel {
  uint32_t subchannel_class[GPU_SUBCHANNELS];
  uint32_t host[0x40];                       /* host method registers */
  uint32_t dma[GPU_DMA_REGISTER_WORDS];      /* B0B5 registers, by word address */
  uint32_t engine3d[GPU_3D_REGISTER_WORDS];  /* B197 registers, by word address */
  uint64_t methods;                          /* diagnostics */
  uint64_t ignored_methods;
  uint64_t dma_copies;
  uint64_t faults;                           /* reads/writes to unmapped GPU VA */
  uint8_t line[GPU_LINE_BYTES];              /* DMA staging */
  uint8_t line_out[GPU_LINE_BYTES];
  uint32_t fetch[GPU_FETCH_WORDS];           /* pushbuffer staging */
} Gpu_Channel;

void gpu_channel_init(Gpu_Channel *channel);

/* Runs `count` GP entries to completion. */
void gpu_channel_submit(Gpu_Channel *channel, const Gpu_Memory *memory, const uint64_t *entries, uint32_t count);

/* One method write, as the pushbuffer decoder issues it (tests). */
void gpu_channel_method(Gpu_Channel *channel, const Gpu_Memory *memory, uint32_t subchannel, uint32_t method,
                        uint32_t data);

#endif /* SWITCH_GPU_GPU_CHANNEL_H */
