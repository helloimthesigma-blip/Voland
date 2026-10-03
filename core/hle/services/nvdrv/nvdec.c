/**
 * Host1x multimedia engines. See nvdec.h.
 */
#include "hle/services/nvdrv/nvdec.h"

#include "common/log.h"
#include "gpu/block_linear.h"
#include "video/host1x.h"
#include "video/vic_convert.h"

#include <string.h>

#define MM_METHOD_EXECUTE 0x300u
#define MM_BUFFER_SHIFT 8u
#define MM_REG(method) ((method) / 4u)

/* NVDEC methods. */
#define NVDEC_APPLICATION_ID 0x200u
#define NVDEC_PIC_SETUP 0x404u
#define NVDEC_BITSTREAM 0x408u
#define NVDEC_PICTURE_INDEX 0x40Cu
#define NVDEC_SLICE_OFFSETS 0x410u
#define NVDEC_OUTPUT_LUMA 0x428u
#define NVDEC_APP_H264 3u

/* VIC methods. */
#define VIC_SLOT0_LUMA 0x400u
#define VIC_CONFIG 0x708u
#define VIC_OUTPUT_LUMA 0x720u
#define VIC_OUTPUT_CHROMA 0x724u
#define VIC_CONFIG_BYTES 0x200u

/* VIC config struct: the output surface word (format + layout) and its
 * sizes, as 14-bit (value - 1) pairs. */
#define VIC_OUT_SURFACE 0x20u
#define VIC_OUT_SIZE 0x24u
#define VIC_OUT_LUMA_SIZE 0x28u
#define VIC_OUT_CHROMA_SIZE 0x2Cu
#define VIC_FORMAT_MASK 0x7Fu
#define VIC_BLOCK_KIND_SHIFT 11u
#define VIC_BLOCK_HEIGHT_SHIFT 15u
#define VIC_NIBBLE 0xFu
#define VIC_SIZE_BITS 14u
#define VIC_SIZE_MASK 0x3FFFu
#define VIC_FORMAT_NV12 0x44u      /* Y8___V8U8_N420 */
#define VIC_FORMAT_A8B8G8R8 0x1Fu  /* T_A8B8G8R8: R,G,B,A in memory */
#define VIC_FORMAT_A8R8G8B8 0x20u  /* T_A8R8G8B8: B,G,R,A in memory */
#define VIC_PITCH_ALIGN 256u

/* nvdec_h264_pic_s (NVIDIA's picture setup), offsets in bytes. */
#define H264_STREAM_LEN 0x48u
#define H264_SLICE_COUNT 0x4Cu
#define H264_LOG2_MAX_POC_LSB 0x58u
#define H264_DELTA_POC_ZERO 0x5Cu
#define H264_FRAME_MBS_ONLY 0x60u
#define H264_WIDTH_MBS 0x64u
#define H264_HEIGHT_MBS 0x68u
#define H264_ENTROPY 0x70u
#define H264_POC_PRESENT 0x74u
#define H264_REF_L0 0x78u
#define H264_REF_L1 0x7Cu
#define H264_DEBLOCK_PRESENT 0x80u
#define H264_REDUNDANT_PRESENT 0x84u
#define H264_TRANSFORM_8X8 0x88u
#define H264_FLAGS 0xB0u   /* MbaffFrameFlag ... pic_order_cnt_type, pic_init_qp ... */
#define H264_FLAGS2 0xB4u  /* weighted_bipred_idc, CurrPicIdx, ... */
#define H264_DPB 0xC0u
#define H264_DPB_ENTRIES 16u
#define H264_DPB_ENTRY_BYTES 16u
#define H264_WEIGHT_SCALE (H264_DPB + H264_DPB_ENTRIES * H264_DPB_ENTRY_BYTES)
#define H264_WEIGHT_SCALE_8X8 (H264_WEIGHT_SCALE + 96u)

#define NAL_TYPE_MASK 0x1Fu
#define NAL_IDR 5u
#define DISCOVERY_DUMPS 3u /* executes whose structs are hex-dumped (offset checks) */
#define DUMP_LINE 16u

/* ---- IOVA table ---------------------------------------------------- */

void mm_iova_init(Mm_Iova *iova) {
  memset(iova, 0, sizeof(*iova));
  iova->next = MM_IOVA_BASE;
}

uint32_t mm_iova_map(Mm_Iova *iova, uint32_t handle, uint64_t guest_va, uint32_t size) {
  Mm_Iova_Map *free_slot = NULL;
  for (uint32_t i = 0; i < MM_MAX_IOVA_MAPS; i++) {
    Mm_Iova_Map *m = &iova->maps[i];
    if (m->handle == handle && handle) {
      if (m->guest_va == guest_va && m->size >= size) return m->iova;
      m->handle = 0; /* the handle was reallocated: map it afresh */
    }
    if (!m->handle && !free_slot) free_slot = m;
  }
  const uint64_t span = ((uint64_t)size + MM_IOVA_ALIGN - 1u) & ~(uint64_t)(MM_IOVA_ALIGN - 1u);
  if (!free_slot || (uint64_t)iova->next + span > UINT32_MAX) return 0;
  free_slot->handle = handle;
  free_slot->iova = iova->next;
  free_slot->size = size;
  free_slot->guest_va = guest_va;
  iova->next += (uint32_t)span;
  return free_slot->iova;
}

void mm_iova_unmap(Mm_Iova *iova, uint32_t handle) {
  for (uint32_t i = 0; i < MM_MAX_IOVA_MAPS; i++) {
    if (iova->maps[i].handle == handle) iova->maps[i].handle = 0;
  }
}

static const Mm_Iova_Map *find_map(const Mm_Iova *iova, uint64_t address) {
  for (uint32_t i = 0; i < MM_MAX_IOVA_MAPS; i++) {
    const Mm_Iova_Map *m = &iova->maps[i];
    if (m->handle && address >= m->iova && address - m->iova < m->size) return m;
  }
  return NULL;
}

bool mm_iova_translate(const Mm_Iova *iova, uint64_t address, uint64_t *guest_va, uint64_t *remaining) {
  const Mm_Iova_Map *m = find_map(iova, address);
  if (!m) return false;
  *guest_va = m->guest_va + (address - m->iova);
  *remaining = m->size - (address - m->iova);
  return true;
}

bool mm_iova_handle(const Mm_Iova *iova, uint64_t address, uint32_t *handle, uint64_t *offset) {
  const Mm_Iova_Map *m = find_map(iova, address);
  if (!m) return false;
  *handle = m->handle;
  *offset = address - m->iova;
  return true;
}

/* ---- Engines ------------------------------------------------------- */

void mm_engine_init(Mm_Engine *engine, uint32_t class_id) {
  memset(engine, 0, sizeof(*engine));
  engine->class_id = class_id;
}

void mm_video_init(Mm_Video *video) {
  memset(&video->params, 0, sizeof(video->params));
  video->configured = false;
  video->references = 0;
  memset(video->surface_iova, 0, sizeof(video->surface_iova));
  memset(video->surface_sequence, 0, sizeof(video->surface_sequence));
  video->next_surface = 0;
  video->frames = video->conversions = video->missing = 0;
}

static uint32_t rd32(const uint8_t *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

static uint32_t field(uint32_t word, uint32_t shift, uint32_t bits) { return (word >> shift) & ((1u << bits) - 1u); }

static int32_t signed_field(uint32_t word, uint32_t shift, uint32_t bits) {
  const uint32_t v = field(word, shift, bits);
  return (v & (1u << (bits - 1u))) ? (int32_t)v - (int32_t)(1u << bits) : (int32_t)v;
}

/* Guest bytes behind a method's buffer register (value << 8). */
static bool read_buffer(const Mm_Context *c, uint32_t value, uint64_t skip, void *out, uint64_t bytes) {
  uint64_t guest = 0, remaining = 0;
  if (!mm_iova_translate(c->iova, ((uint64_t)value << MM_BUFFER_SHIFT) + skip, &guest, &remaining)) return false;
  if (bytes > remaining) return false;
  return error_is_ok(vmm_read_block(c->vmm, guest, out, bytes));
}

static void hexdump(const char *what, const uint8_t *bytes, uint32_t n) {
  static const char hex[] = "0123456789abcdef";
  log_info("[video] %s:", what);
  for (uint32_t at = 0; at < n; at += DUMP_LINE) {
    char line[DUMP_LINE * 3u + 1u];
    for (uint32_t k = 0; k < DUMP_LINE; k++) {
      line[k * 3u] = hex[bytes[at + k] >> 4];
      line[k * 3u + 1u] = hex[bytes[at + k] & 0xFu];
      line[k * 3u + 2u] = ' ';
    }
    line[DUMP_LINE * 3u] = 0;
    log_info("[video]   +%03x %s", at, line);
  }
}

/* ---- NVDEC: H.264 --------------------------------------------------- */

/* WeightScale lists are raster order in the struct; the PPS codes them
 * in zig-zag (H.264 8.5.6). */
static const uint8_t k_zigzag4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static const uint8_t k_zigzag8[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                      12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                      35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                      58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
#define FLAT_SCALE 16u

void nvdec_h264_params(const uint8_t *s, H264_Params *p) {
  memset(p, 0, sizeof(*p));
  const uint32_t flags = rd32(s + H264_FLAGS), flags2 = rd32(s + H264_FLAGS2);
  p->width_mbs = rd32(s + H264_WIDTH_MBS);
  p->height_mbs = rd32(s + H264_HEIGHT_MBS);
  p->log2_max_poc_lsb_minus4 = rd32(s + H264_LOG2_MAX_POC_LSB);
  p->delta_pic_order_always_zero = rd32(s + H264_DELTA_POC_ZERO) != 0;
  p->frame_mbs_only = rd32(s + H264_FRAME_MBS_ONLY) != 0;
  p->entropy_coding_mode = rd32(s + H264_ENTROPY) != 0;
  p->bottom_field_pic_order_in_frame_present = rd32(s + H264_POC_PRESENT) != 0;
  p->num_ref_idx_l0_default_minus1 = rd32(s + H264_REF_L0);
  p->num_ref_idx_l1_default_minus1 = rd32(s + H264_REF_L1);
  p->deblocking_filter_control_present = rd32(s + H264_DEBLOCK_PRESENT) != 0;
  p->redundant_pic_cnt_present = rd32(s + H264_REDUNDANT_PRESENT) != 0;
  p->transform_8x8_mode = rd32(s + H264_TRANSFORM_8X8) != 0;
  /* flags: MbaffFrameFlag:1 direct_8x8:1 weighted_pred:1 constrained_intra:1
   * ref_pic:1 field_pic:1 bottom_field:1 second_field:1
   * log2_max_frame_num_minus4:4 chroma_format_idc:2 pic_order_cnt_type:2
   * pic_init_qp_minus26:6 chroma_qp_index_offset:5 second_chroma_qp_index_offset:5
   * weighted_bipred_idc:2 */
  p->direct_8x8_inference = field(flags, 1, 1) != 0;
  p->weighted_pred = field(flags, 2, 1) != 0;
  p->constrained_intra_pred = field(flags, 3, 1) != 0;
  p->log2_max_frame_num_minus4 = field(flags, 8, 4);
  p->chroma_format_idc = field(flags, 12, 2);
  p->pic_order_cnt_type = field(flags, 14, 2);
  p->pic_init_qp_minus26 = signed_field(flags, 16, 6);
  p->chroma_qp_index_offset = signed_field(flags, 22, 5);
  p->second_chroma_qp_index_offset = signed_field(flags, 27, 5);
  p->weighted_bipred_idc = field(flags2, 0, 2);
  if (!p->chroma_format_idc) p->chroma_format_idc = 1; /* 4:2:0 is all NVDEC outputs */
  /* max_num_ref_frames is not in the struct, and it must be exact: a
   * sliding window wider than the stream's keeps stale references that
   * shift B-slice reference lists. Encoders (x264) size it from the
   * default list length; nvdec_h264 widens it when the guest's DPB holds
   * more references than that. */
  p->max_num_ref_frames = p->num_ref_idx_l0_default_minus1 + 1u;
  for (uint32_t i = 0; i < H264_SCALING_4X4_LISTS; i++) {
    for (uint32_t k = 0; k < 16u; k++) {
      p->scaling_4x4[i][k] = s[H264_WEIGHT_SCALE + i * 16u + k_zigzag4[k]];
      if (p->scaling_4x4[i][k] != FLAT_SCALE) p->scaling_matrix_present = true;
    }
  }
  for (uint32_t i = 0; i < H264_SCALING_8X8_LISTS; i++) {
    for (uint32_t k = 0; k < 64u; k++) {
      p->scaling_8x8[i][k] = s[H264_WEIGHT_SCALE_8X8 + i * 64u + k_zigzag8[k]];
      if (p->transform_8x8_mode && p->scaling_8x8[i][k] != FLAT_SCALE) p->scaling_matrix_present = true;
    }
  }
}

uint32_t nvdec_h264_stream_length(const uint8_t *setup) { return rd32(setup + H264_STREAM_LEN); }

/* DPB entries: index:7 col_idx:5 state:2 long_term:1 not_existing:1
 * is_field:1 top_marking:4 bottom_marking:4 ...; marked = a reference. */
#define DPB_MARKING_SHIFT 17u
#define DPB_MARKING_MASK 0xFFu
uint32_t nvdec_h264_references(const uint8_t *setup) {
  uint32_t count = 0;
  for (uint32_t i = 0; i < H264_DPB_ENTRIES; i++) {
    if ((rd32(setup + H264_DPB + i * H264_DPB_ENTRY_BYTES) >> DPB_MARKING_SHIFT) & DPB_MARKING_MASK) count++;
  }
  return count;
}

static bool has_idr(const uint8_t *data, uint32_t bytes) {
  for (uint32_t i = 0; i + 3u < bytes; i++) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && (data[i + 3] & NAL_TYPE_MASK) == NAL_IDR) return true;
  }
  return false;
}

static void remember_surface(Mm_Video *v, uint32_t luma, uint32_t sequence) {
  for (uint32_t i = 0; i < MM_MAX_SURFACES; i++) {
    if (v->surface_iova[i] == luma) {
      v->surface_sequence[i] = sequence;
      return;
    }
  }
  const uint32_t i = v->next_surface++ % MM_MAX_SURFACES;
  v->surface_iova[i] = luma;
  v->surface_sequence[i] = sequence;
}

static bool surface_sequence(const Mm_Video *v, uint32_t luma, uint32_t *sequence) {
  for (uint32_t i = 0; i < MM_MAX_SURFACES; i++) {
    if (v->surface_iova[i] == luma && luma) {
      *sequence = v->surface_sequence[i];
      return true;
    }
  }
  return false;
}

static void nvdec_h264(Mm_Engine *e, const Mm_Context *c) {
  static uint8_t setup[NVDEC_PIC_SETUP_BYTES];
  Mm_Video *v = c->state;
  if (!read_buffer(c, e->regs[MM_REG(NVDEC_PIC_SETUP)], 0, setup, sizeof(setup))) return;
  if (e->executes <= DISCOVERY_DUMPS) {
    hexdump("nvdec picture setup", setup, sizeof(setup) / 2u);
    uint8_t slices[DUMP_LINE * 2u];
    if (read_buffer(c, e->regs[MM_REG(NVDEC_SLICE_OFFSETS)], 0, slices, sizeof(slices))) {
      hexdump("nvdec slice offsets", slices, sizeof(slices));
    }
  }
  H264_Params params;
  nvdec_h264_params(setup, &params);
  /* A wider reference window only takes effect at a key frame (a new
   * decoder needs one); until then the stream keeps its current one. */
  const uint32_t references = nvdec_h264_references(setup);
  if (references > v->references) v->references = references;
  if (v->references > params.max_num_ref_frames) params.max_num_ref_frames = v->references;
  uint32_t length = nvdec_h264_stream_length(setup);
  uint8_t head[DUMP_LINE * 4u];
  const uint32_t peek = length < sizeof(head) ? length : (uint32_t)sizeof(head);
  const bool idr = read_buffer(c, e->regs[MM_REG(NVDEC_BITSTREAM)], 0, head, peek) && has_idr(head, peek);
  if (v->configured && !idr) params.max_num_ref_frames = v->params.max_num_ref_frames;
  if (!v->configured || !h264_params_equal(&params, &v->params)) {
    v->params = params;
    v->configured = true;
    char codec[16];
    h264_codec_string(codec);
    log_info("[video] H.264 %ux%u MBs: CABAC %d, poc type %u, log2 frame num %u, 8x8 %d, refs %u/%u, wp %d/%u",
             params.width_mbs, params.height_mbs, params.entropy_coding_mode, params.pic_order_cnt_type,
             params.log2_max_frame_num_minus4 + 4u, params.transform_8x8_mode,
             params.num_ref_idx_l0_default_minus1 + 1u, params.num_ref_idx_l1_default_minus1 + 1u,
             params.weighted_pred, params.weighted_bipred_idc);
    if (c->video) video_configure(c->video, params.width_mbs * 16u, params.height_mbs * 16u, codec);
  }
  const size_t sets = h264_write_parameter_sets(&v->params, v->access_unit, sizeof(v->access_unit));
  if (!sets || length > sizeof(v->access_unit) - sets) {
    log_warn("[video] H.264 picture of %u bytes dropped", length);
    return;
  }
  if (!read_buffer(c, e->regs[MM_REG(NVDEC_BITSTREAM)], 0, v->access_unit + sets, length)) return;
  v->frames++;
  if (!c->video) return;
  const uint32_t sequence = video_decode(c->video, idr, v->access_unit, (uint32_t)(sets + length));
  remember_surface(v, e->regs[MM_REG(NVDEC_OUTPUT_LUMA)], sequence);
}

/* ---- VIC ------------------------------------------------------------ */

typedef struct Vic_Output {
  uint32_t format, block_kind, block_height_log2;
  uint32_t width, height;
} Vic_Output;

static void vic_output(const uint8_t *config, Vic_Output *o) {
  const uint32_t surface = rd32(config + VIC_OUT_SURFACE), size = rd32(config + VIC_OUT_SIZE);
  o->format = surface & VIC_FORMAT_MASK;
  o->block_kind = field(surface, VIC_BLOCK_KIND_SHIFT, 4);
  o->block_height_log2 = field(surface, VIC_BLOCK_HEIGHT_SHIFT, 4);
  o->width = field(size, 0, VIC_SIZE_BITS) + 1u;
  o->height = field(size, VIC_SIZE_BITS, VIC_SIZE_BITS) + 1u;
}

/* A plane (rows of `row_bytes` from src with src_pitch) to guest memory
 * at the buffer register `value`: pitch-linear or block-linear. */
static bool write_plane(const Mm_Context *c, uint32_t value, const uint8_t *src, uint32_t src_pitch, uint32_t row_bytes,
                        uint32_t rows, const Vic_Output *o, uint8_t *scratch, uint64_t scratch_bytes) {
  uint64_t guest = 0, remaining = 0;
  const uint64_t iova = (uint64_t)value << MM_BUFFER_SHIFT;
  if (!mm_iova_translate(c->iova, iova, &guest, &remaining)) return false;
  const uint32_t pitch = (row_bytes + VIC_PITCH_ALIGN - 1u) & ~(VIC_PITCH_ALIGN - 1u);
  uint64_t bytes;
  if (o->block_kind) {
    bytes = block_linear_size(row_bytes, rows, o->block_height_log2);
    if (bytes > scratch_bytes || bytes > remaining) return false;
    memset(scratch, 0, bytes);
    pitch_to_block_linear(src, src_pitch, scratch, row_bytes, rows, o->block_height_log2);
  } else {
    bytes = (uint64_t)pitch * rows;
    if (bytes > scratch_bytes || bytes > remaining) return false;
    for (uint32_t r = 0; r < rows; r++) memcpy(scratch + (uint64_t)r * pitch, src + (uint64_t)r * src_pitch, row_bytes);
  }
  if (!error_is_ok(vmm_write_block(c->vmm, guest, scratch, bytes))) return false;
  uint32_t handle = 0;
  uint64_t offset = 0;
  if (c->written && mm_iova_handle(c->iova, iova, &handle, &offset)) c->written(c->written_user, handle, offset, bytes);
  return true;
}

#define VIC_SCRATCH_BYTES ((uint64_t)VIDEO_MAX_WIDTH * 4u * (VIDEO_MAX_HEIGHT + 128u))

static void vic_execute(Mm_Engine *e, const Mm_Context *c) {
  static uint8_t config[VIC_CONFIG_BYTES];
  static uint8_t scratch[VIC_SCRATCH_BYTES];
  static uint8_t rgba[(uint64_t)VIDEO_MAX_WIDTH * VIDEO_MAX_HEIGHT * 4u];
  Mm_Video *v = c->state;
  if (!read_buffer(c, e->regs[MM_REG(VIC_CONFIG)], 0, config, sizeof(config))) return;
  if (e->executes <= DISCOVERY_DUMPS) hexdump("vic config", config, sizeof(config));
  uint32_t sequence = 0;
  Video_Frame frame;
  if (!c->video || !surface_sequence(v, e->regs[MM_REG(VIC_SLOT0_LUMA)], &sequence) ||
      !video_frame_for(c->video, sequence, &frame)) {
    v->missing++;
    return;
  }
  Vic_Output o;
  vic_output(config, &o);
  if (o.width > VIDEO_MAX_WIDTH || o.height > VIDEO_MAX_HEIGHT) return;
  const uint32_t w = o.width < frame.width ? o.width : frame.width;
  const uint32_t h = o.height < frame.height ? o.height : frame.height;
  bool ok = false;
  if (o.format == VIC_FORMAT_NV12) {
    ok = write_plane(c, e->regs[MM_REG(VIC_OUTPUT_LUMA)], frame.luma, frame.pitch, w, h, &o, scratch,
                     sizeof(scratch)) &&
         write_plane(c, e->regs[MM_REG(VIC_OUTPUT_CHROMA)], frame.chroma, frame.pitch, w, h / 2u, &o, scratch,
                     sizeof(scratch));
  } else if (o.format == VIC_FORMAT_A8B8G8R8 || o.format == VIC_FORMAT_A8R8G8B8) {
    const Vic_Nv12 source = {frame.luma, frame.chroma, frame.width, frame.height, frame.pitch};
    const Vic_Target target = {rgba, o.width, o.height, o.width * 4u, false, 0, o.format == VIC_FORMAT_A8R8G8B8,
                               0xFF};
    vic_convert_nv12(&source, &target, VIC_MATRIX_BT709);
    ok = write_plane(c, e->regs[MM_REG(VIC_OUTPUT_LUMA)], rgba, o.width * 4u, o.width * 4u, o.height, &o, scratch,
                     sizeof(scratch));
  } else if (e->executes <= DISCOVERY_DUMPS) {
    log_warn("[video] VIC output format 0x%x not handled", o.format);
  }
  if (ok) {
    v->conversions++;
    video_frame_used(c->video, &frame);
  }
}

/* ---- Dispatch ------------------------------------------------------- */

typedef struct Submit_Ctx {
  Mm_Engine *engine;
  const Mm_Context *context;
} Submit_Ctx;

static void execute(Mm_Engine *e, const Mm_Context *c) {
  e->executes++;
  if (e->class_id == HOST1X_CLASS_NVDEC) {
    if (e->regs[MM_REG(NVDEC_APPLICATION_ID)] == NVDEC_APP_H264) {
      nvdec_h264(e, c);
    } else if (e->executes <= DISCOVERY_DUMPS) {
      log_warn("[video] NVDEC application %u not decoded", e->regs[MM_REG(NVDEC_APPLICATION_ID)]);
    }
  } else if (e->class_id == HOST1X_CLASS_VIC) {
    vic_execute(e, c);
  }
}

static void on_method(void *user, uint32_t class_id, uint32_t method, uint32_t value) {
  Submit_Ctx *ctx = (Submit_Ctx *)user;
  if (method & 0x80000000u) return; /* host-class register (syncpoint increments) */
  if (class_id != ctx->engine->class_id || method / 4u >= MM_ENGINE_REGS) return;
  ctx->engine->regs[method / 4u] = value;
  if (method == MM_METHOD_EXECUTE && ctx->context->state) execute(ctx->engine, ctx->context);
}

void mm_engine_submit(Mm_Engine *engine, const Mm_Context *context, const uint32_t *words, uint32_t count) {
  Submit_Ctx ctx = {engine, context};
  Host1x_Method_Latch latch = {0, on_method, &ctx};
  Host1x_Parser parser;
  host1x_parser_init(&parser);
  if (!host1x_parse(&parser, words, count, host1x_latch_write, &latch)) {
    log_warn("[video] class 0x%x: unparsed host1x opcode", engine->class_id);
  }
}
