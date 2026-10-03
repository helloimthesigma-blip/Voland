/**
 * Host1x multimedia engines behind /dev/nvhost-nvdec and /dev/nvhost-vic
 * (DESIGN §13 "Video decode"). A channel SUBMIT's command buffers are
 * parsed (video/host1x.h) into engine method writes, which land in a
 * per-engine register file; EXECUTE (method 0x300) runs the engine.
 *
 * Buffers are named by IOVAs: MAP_BUFFER hands the guest a 32-bit
 * address per nvmap handle, and engine methods carry those addresses
 * shifted right by 8. Mm_Iova resolves them back to guest VAs.
 *
 * NVDEC methods (as the guest drives them; docs/handoff/BOT_2_STATUS.md):
 *   0x200 application id (3 = H.264)    0x400 control params
 *   0x404 picture setup                  0x408 bitstream
 *   0x40C picture index                  0x410 slice offsets {u32 offset, u32 size}
 *   0x428/0x42C output luma/chroma       0x430.. reference surfaces
 * EXECUTE turns the picture into an Annex-B access unit (synthesized
 * SPS/PPS + the slices) for the platform decoder (video/video_stream.h)
 * and remembers which decode sequence lands in which output surface.
 *
 * VIC methods:
 *   0x400/0x404 slot 0 input luma/chroma  0x708 config struct
 *   0x720/0x724 output luma/chroma
 * EXECUTE writes the decoded frame meant for its input surface into the
 * output surface in the format the config struct names (NV12 pitch or
 * block-linear, or RGBA8).
 */
#ifndef SWITCH_HLE_SERVICES_NVDRV_NVDEC_H
#define SWITCH_HLE_SERVICES_NVDRV_NVDEC_H

#include <stdbool.h>
#include <stdint.h>

#include "common/vmm.h"
#include "video/h264.h"
#include "video/video_stream.h"

#define MM_ENGINE_REGS 0x400u /* methods 0..0xFFC */
#define MM_MAX_IOVA_MAPS 256u
#define MM_IOVA_BASE 0x00100000u
#define MM_IOVA_ALIGN 0x00100000u /* 1 MiB: room between buffers, low bits free */
#define MM_MAX_SURFACES 32u       /* output surfaces remembered (luma IOVA -> sequence) */
#define MM_MAX_ACCESS_UNIT (2u * 1024u * 1024u)

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
/* IOVA -> the nvmap handle and the offset into it. */
bool mm_iova_handle(const Mm_Iova *iova, uint64_t address, uint32_t *handle, uint64_t *offset);

typedef struct Mm_Engine {
  uint32_t class_id;
  uint32_t regs[MM_ENGINE_REGS];
  uint64_t executes;
} Mm_Engine;

/* Called after VIC wrote `bytes` at guest_va (nvmap `handle` + offset):
 * the renderer re-reads textures there. */
typedef void (*Mm_Written)(void *user, uint32_t handle, uint64_t offset, uint64_t bytes);

/* The decode state shared by NVDEC and VIC. */
typedef struct Mm_Video {
  H264_Params params;  /* the stream's parameter sets (a change reconfigures) */
  bool configured;
  uint32_t references; /* the most references the guest's DPB has held (sizes max_num_ref_frames) */
  uint32_t surface_iova[MM_MAX_SURFACES];
  uint32_t surface_sequence[MM_MAX_SURFACES];
  uint32_t next_surface;
  uint64_t frames, conversions, missing; /* diagnostics */
  uint8_t access_unit[MM_MAX_ACCESS_UNIT];
} Mm_Video;

typedef struct Mm_Context {
  VMM_Context *vmm;
  const Mm_Iova *iova;
  Video_Stream *video;  /* NULL: engines run, nothing decodes */
  Mm_Video *state;
  Mm_Written written;
  void *written_user;
} Mm_Context;

void mm_engine_init(Mm_Engine *engine, uint32_t class_id);
void mm_video_init(Mm_Video *video);

/* Runs one command buffer's words on `engine`. */
void mm_engine_submit(Mm_Engine *engine, const Mm_Context *context, const uint32_t *words, uint32_t count);

/* ---- Exposed for tests -------------------------------------------- */

#define NVDEC_PIC_SETUP_BYTES 0x400u

/* H.264 parameters from a picture-setup struct (NVDEC_PIC_SETUP_BYTES). */
void nvdec_h264_params(const uint8_t *setup, H264_Params *out);
/* The bitstream's length per the picture setup. */
uint32_t nvdec_h264_stream_length(const uint8_t *setup);
/* Reference frames in the picture's DPB. */
uint32_t nvdec_h264_references(const uint8_t *setup);

#endif /* SWITCH_HLE_SERVICES_NVDRV_NVDEC_H */
