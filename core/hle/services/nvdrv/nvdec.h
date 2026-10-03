/**
 * Host1x multimedia engines behind /dev/nvhost-nvdec and /dev/nvhost-vic
 * (DESIGN §13 "Video decode"). A channel SUBMIT's command buffers are
 * parsed (video/host1x.h) into engine method writes, which land in a
 * per-engine register file; EXECUTE runs the engine on it.
 *
 * Buffers are named by IOVAs: MAP_BUFFER hands the guest a 32-bit
 * address per nvmap handle, and engine methods carry those addresses
 * shifted right by 8. Mm_Iova resolves them back to guest VAs.
 */
#ifndef SWITCH_HLE_SERVICES_NVDRV_NVDEC_H
#define SWITCH_HLE_SERVICES_NVDRV_NVDEC_H

#include <stdbool.h>
#include <stdint.h>

#include "common/vmm.h"

#define MM_ENGINE_REGS 0x400u /* methods 0..0xFFC */
#define MM_MAX_IOVA_MAPS 256u
#define MM_IOVA_BASE 0x00100000u
#define MM_IOVA_ALIGN 0x00100000u /* 1 MiB: room between buffers, low bits free */

typedef struct Mm_Iova_Map {
  uint32_t handle; /* nvmap handle; 0 = free slot */
  uint32_t iova;
  uint32_t size;
  uint64_t guest_va;
} Mm_Iova_Map;

typedef struct Mm_Iova {
  Mm_Iova_Map maps[MM_MAX_IOVA_MAPS];
  uint32_t next;
} Mm_Iova;

void mm_iova_init(Mm_Iova *iova);
/* The IOVA for an nvmap handle backed by guest_va/size (stable per handle
 * while the backing stays the same). 0 when the table is full. */
uint32_t mm_iova_map(Mm_Iova *iova, uint32_t handle, uint64_t guest_va, uint32_t size);
void mm_iova_unmap(Mm_Iova *iova, uint32_t handle);
/* IOVA -> guest VA and the bytes left in that buffer. */
bool mm_iova_translate(const Mm_Iova *iova, uint64_t address, uint64_t *guest_va, uint64_t *remaining);

typedef struct Mm_Engine {
  uint32_t class_id;
  uint32_t regs[MM_ENGINE_REGS];
  uint64_t executes;
} Mm_Engine;

typedef struct Mm_Context {
  VMM_Context *vmm;
  const Mm_Iova *iova;
} Mm_Context;

void mm_engine_init(Mm_Engine *engine, uint32_t class_id);

/* Runs one command buffer's words on `engine`. */
void mm_engine_submit(Mm_Engine *engine, const Mm_Context *context, const uint32_t *words, uint32_t count);

#endif /* SWITCH_HLE_SERVICES_NVDRV_NVDEC_H */
