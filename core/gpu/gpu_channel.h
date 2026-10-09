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
 *   - Maxwell 3D (class B197), the non-rendering half: the register file
 *     is kept; SYNCPT_ACTION increments, REPORT_SEMAPHORE releases and
 *     LOAD_CONSTANT_BUFFER uploads execute; and the Macro Method Expander
 *     (MME) runs - macros are uploaded with LOAD_MME_* and invoked by
 *     CALL_MME_MACRO(j)/CALL_MME_DATA(j) (methods 0xE00+2j / +1); a macro
 *     runs once its call's data ends (a different method, or the end of
 *     the submission) and the methods it emits go through the same 3D
 *     method path. deko3d issues most of its state and every draw this
 *     way. MME ISA: the Fermi-family macro processor (behaviour per
 *     envytools / Mesa's MIT-licensed simulator; Voland's own code).
 * 3D clears and draws (CLEAR_SURFACE, DRAW_VERTEX_ARRAY, DRAW_INDEX_BUFFER
 * and their BEGIN_END forms, inline indices) run on the reference
 * renderer (gpu/raster3d) when the memory interface provides one; it
 * renders into guest memory and is flushed at the end of each
 * submission and before DMA copies. Inline-to-memory uploads (methods
 * 0x60-0x6D of the 3D, compute and I2M classes: LAUNCH_DMA then
 * LOAD_INLINE_DATA words) are written to pitch or block-linear
 * destinations. Compute dispatch and 2D are counted and ignored.
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

#include "gpu/compute.h"
#include "gpu/raster3d.h"

#define GPU_SUBCHANNELS 8u
#define GPU_DMA_REGISTER_WORDS 0x200u /* B0B5 method space through 0x7FC */
#define GPU_2D_REGISTER_WORDS 0x240u  /* 902D method space through 0x8FC */
#define GPU_3D_REGISTER_WORDS 0x1000u /* B197 method space (0x4000 bytes) */
#define GPU_MME_CODE_WORDS 0x1000u
#define GPU_MME_MACROS 0x80u
#define GPU_MME_MAX_PARAMS 0x4000u
#define GPU_LINE_BYTES 0x10000u       /* longest DMA line handled */
#define GPU_FETCH_WORDS 0x1000u       /* pushbuffer words fetched at a time */
#define GPU_INLINE_INDICES 0x4000u    /* DRAW_INLINE_INDEX words between BEGIN and END */
#define GPU_I2M_FIRST 0x60u           /* inline-to-memory methods (3D, compute, I2M classes) */
#define GPU_I2M_WORDS 0x0Eu

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
  /* The 3D reference renderer (gpu/raster3d). NULL: draws and clears
   * are counted and skipped. */
  Raster3d *renderer;
  /* The guest (CPU) address of GPU address `gpu_va`; may be NULL. The
   * WebGPU renderer matches presented buffers (CPU addresses) to the
   * render targets (GPU addresses) that hold them. */
  bool (*translate)(void *user, uint64_t gpu_va, uint64_t *guest_va);
  /* Bytes from `gpu_va` that stay inside its mapping (0: unmapped); may
   * be NULL. Sizes the storage buffers a compute dispatch leaves unsized. */
  uint64_t (*extent)(void *user, uint64_t gpu_va);
} Gpu_Memory;

typedef struct Gpu_Channel {
  uint32_t subchannel_class[GPU_SUBCHANNELS];
  uint32_t host[0x40];                       /* host method registers */
  uint32_t dma[GPU_DMA_REGISTER_WORDS];      /* B0B5 registers, by word address */
  uint32_t engine3d[GPU_3D_REGISTER_WORDS];  /* B197 registers, by word address */
  uint32_t engine2d[GPU_2D_REGISTER_WORDS];  /* 902D registers, by word address */
  uint32_t compute[COMPUTE_REGISTER_WORDS];  /* B1C0 registers, by word address */
  uint64_t compute_launches;
  /* Macro Method Expander. */
  uint32_t mme_code[GPU_MME_CODE_WORDS];
  uint32_t mme_start[GPU_MME_MACROS];
  bool mme_pending;                          /* a CALL_MME_MACRO is collecting data */
  uint32_t mme_macro;
  uint32_t mme_param_count;
  uint32_t mme_params[GPU_MME_MAX_PARAMS];
  /* 3D draw state the register file does not hold. */
  Raster3d_Bindings bindings;                /* BIND_GROUP_CONSTANT_BUFFER */
  uint32_t draw_topology;                    /* BEGIN */
  uint32_t draw_instance;
  uint32_t inline_count;
  uint32_t inline_indices[GPU_INLINE_INDICES];
  uint64_t draws;
  /* Inline-to-memory (LAUNCH_DMA + LOAD_INLINE_DATA): shader code and
   * small uploads written straight from the pushbuffer. */
  uint32_t i2m[GPU_I2M_WORDS];
  bool i2m_active;
  uint64_t i2m_received;                     /* bytes so far */
  uint32_t i2m_line_fill;
  uint64_t i2m_uploads;
  uint64_t mme_runs;
  uint64_t mme_faults;                       /* runaway or out-of-range macros */
  uint64_t methods;                          /* diagnostics */
  uint64_t ignored_methods;
  uint64_t dma_copies;
  uint64_t blits;                            /* 2D PIXELS_FROM_MEMORY */
  uint64_t faults;                           /* reads/writes to unmapped GPU VA */
  uint8_t line[GPU_LINE_BYTES];              /* DMA staging */
  uint8_t line_out[GPU_LINE_BYTES];
  uint32_t fetch[GPU_FETCH_WORDS];           /* pushbuffer staging */
} Gpu_Channel;

void gpu_channel_init(Gpu_Channel *channel);

/* Runs `count` GP entries to completion. */
void gpu_channel_submit(Gpu_Channel *channel, const Gpu_Memory *memory, const uint64_t *entries, uint32_t count);
/* The same, decoding `captured` - every entry's words, consecutively, as
 * gpu_channel_capture read them - instead of reading guest memory now: the
 * asynchronous GPU thread runs a submission after the guest may have
 * reused its command memory (docs/ASYNC_GPU.md "Captured commands"). */
void gpu_channel_submit_words(Gpu_Channel *channel, const Gpu_Memory *memory, const uint64_t *entries, uint32_t count,
                              const uint32_t *captured);
/* A GPFIFO entry's length in words; the entries' words into `out` (false:
 * more than `capacity` words, or unreadable). */
uint32_t gpu_channel_entry_words(uint64_t entry);
bool gpu_channel_capture(const Gpu_Memory *memory, const uint64_t *entries, uint32_t count, uint32_t *out,
                         uint32_t capacity);

/* One method write, as the pushbuffer decoder issues it (tests). */
void gpu_channel_method(Gpu_Channel *channel, const Gpu_Memory *memory, uint32_t subchannel, uint32_t method,
                        uint32_t data);

/* Runs a macro call still collecting data (gpu_channel_submit does this
 * at the end of every submission; tests driving methods call it). */
void gpu_channel_flush_macro(Gpu_Channel *channel, const Gpu_Memory *memory);

#endif /* SWITCH_GPU_GPU_CHANNEL_H */
