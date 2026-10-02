/**
 * GPU channel: GPFIFO / pushbuffer decode, host methods, and the Maxwell
 * DMA copy engine. See gpu_channel.h.
 */
#include "gpu/gpu_channel.h"

#include "gpu/block_linear.h"

#include <string.h>

/* GP entry. */
#define GP_VA_MASK 0xFFFFFFFFFCull
#define GP_LENGTH_SHIFT 42u
#define GP_LENGTH_MASK 0x1FFFFFu

/* Method header. */
#define MH_METHOD(w) ((w) & 0x1FFFu)
#define MH_SUBCHANNEL(w) (((w) >> 13) & 7u)
#define MH_COUNT(w) (((w) >> 16) & 0x1FFFu)
#define MH_OP(w) ((w) >> 29)
#define OP_INCREMENTING 1u
#define OP_NON_INCREMENTING 3u
#define OP_IMMEDIATE 4u
#define OP_INCREMENT_ONCE 5u

/* Host methods (word addresses). */
#define HOST_METHODS 0x40u
#define HOST_BIND 0x00u
#define HOST_SEMAPHORE_HI 0x04u
#define HOST_SEMAPHORE_LO 0x05u
#define HOST_SEMAPHORE_PAYLOAD 0x06u
#define HOST_SEMAPHORE_EXECUTE 0x07u
#define HOST_SYNCPOINT_OP 0x1Du
#define SEMAPHORE_OP_MASK 7u
#define SEMAPHORE_OP_RELEASE 2u
#define SEMAPHORE_RELEASE_4BYTE (1u << 24)
#define SYNCPOINT_OP_INCREMENT 1u
#define SYNCPOINT_ID(d) (((d) >> 8) & 0xFFFu)
#define CLASS_MASK 0xFFFFu

/* B0B5 registers (byte offset / 4). */
#define DMA_SEMAPHORE_A (0x240u / 4u)
#define DMA_SEMAPHORE_B (0x244u / 4u)
#define DMA_SEMAPHORE_PAYLOAD (0x248u / 4u)
#define DMA_LAUNCH (0x300u / 4u)
#define DMA_OFFSET_IN_UPPER (0x400u / 4u)
#define DMA_OFFSET_IN_LOWER (0x404u / 4u)
#define DMA_OFFSET_OUT_UPPER (0x408u / 4u)
#define DMA_OFFSET_OUT_LOWER (0x40Cu / 4u)
#define DMA_PITCH_IN (0x410u / 4u)
#define DMA_PITCH_OUT (0x414u / 4u)
#define DMA_LINE_LENGTH_IN (0x418u / 4u)
#define DMA_LINE_COUNT (0x41Cu / 4u)
#define DMA_REMAP_CONST_A (0x700u / 4u)
#define DMA_REMAP_CONST_B (0x704u / 4u)
#define DMA_REMAP_COMPONENTS (0x708u / 4u)
#define DMA_DST_BLOCK_SIZE (0x70Cu / 4u)
#define DMA_DST_WIDTH (0x710u / 4u)
#define DMA_DST_HEIGHT (0x714u / 4u)
#define DMA_DST_ORIGIN (0x720u / 4u)
#define DMA_SRC_BLOCK_SIZE (0x728u / 4u)
#define DMA_SRC_WIDTH (0x72Cu / 4u)
#define DMA_SRC_HEIGHT (0x730u / 4u)
#define DMA_SRC_ORIGIN (0x73Cu / 4u)

/* B197 (word addresses; NVIDIA clb197.h byte offsets / 4). */
#define M3D_LOAD_MME_INSTRUCTION_RAM_POINTER 0x45u
#define M3D_LOAD_MME_INSTRUCTION_RAM 0x46u
#define M3D_LOAD_MME_START_ADDRESS_RAM_POINTER 0x47u
#define M3D_LOAD_MME_START_ADDRESS_RAM 0x48u
#define M3D_CB_SELECTOR_SIZE 0x8E0u
#define M3D_CB_SELECTOR_ADDRESS_HI 0x8E1u
#define M3D_CB_SELECTOR_ADDRESS_LO 0x8E2u
#define M3D_CB_LOAD_OFFSET 0x8E3u
#define M3D_CB_LOAD_FIRST 0x8E4u
#define M3D_CB_LOAD_LAST 0x8F3u
#define M3D_MACRO_FIRST 0xE00u
#define M3D_SYNCPT_ACTION 0xB2u
#define M3D_REPORT_SEMAPHORE_A 0x6C0u
#define M3D_REPORT_SEMAPHORE_B 0x6C1u
#define M3D_REPORT_SEMAPHORE_C 0x6C2u
#define M3D_REPORT_SEMAPHORE_D 0x6C3u
#define M3D_VERTEX_ARRAY_START 0x35Du
#define M3D_DRAW_VERTEX_ARRAY 0x35Eu
#define M3D_DRAW_VERTEX_ARRAY_FIRST 0x485u       /* ..._BEGIN_END_INSTANCE_FIRST */
#define M3D_DRAW_VERTEX_ARRAY_SUBSEQUENT 0x486u
#define M3D_DRAW_INLINE_INDEX 0x57Au
#define M3D_DRAW_INLINE_INDEX2X16 0x57Cu
#define M3D_END 0x585u
#define M3D_BEGIN 0x586u
#define M3D_INDEX_BUFFER_E 0x5F6u
#define M3D_INDEX_BUFFER_F 0x5F7u
#define M3D_DRAW_INDEX_BUFFER 0x5F8u
#define M3D_DRAW_INDEX32_FIRST 0x5F9u             /* 32/16/8-bit FIRST, then SUBSEQUENT */
#define M3D_DRAW_INDEX8_SUBSEQUENT 0x5FEu
#define M3D_CLEAR_SURFACE 0x674u
#define M3D_BIND_GROUP_CB 0x904u                  /* + 8 * group */
#define M3D_BIND_GROUP_STRIDE 8u
#define M3D_BEGIN_TOPOLOGY(d) ((d) & 0xFFFFu)
#define M3D_BEGIN_INSTANCE(d) (((d) >> 26) & 3u)  /* 0 first, 1 subsequent, 2 unchanged */
#define M3D_COMPACT_FIRST(d) ((d) & 0xFFFFu)
#define M3D_COMPACT_COUNT(d) (((d) >> 16) & 0xFFFu)
#define M3D_COMPACT_TOPOLOGY(d) ((d) >> 28)
#define M3D_SYNCPT_ID(d) ((d) & 0xFFFFu)
#define M3D_SYNCPT_INCREMENT (1u << 16)
#define M3D_REPORT_OPERATION_MASK 3u
#define M3D_REPORT_RELEASE 0u
#define M3D_REPORT_COUNTER 2u
#define M3D_REPORT_ONE_WORD (1u << 28)

/* LAUNCH_DMA fields. */
#define LAUNCH_TRANSFER_MASK 3u
#define LAUNCH_SEMAPHORE_SHIFT 3u
#define LAUNCH_SEMAPHORE_MASK 3u
#define LAUNCH_SEMAPHORE_ONE_WORD 1u
#define LAUNCH_SEMAPHORE_FOUR_WORD 2u
#define LAUNCH_SRC_PITCH (1u << 7)
#define LAUNCH_DST_PITCH (1u << 8)
#define LAUNCH_MULTI_LINE (1u << 9)
#define LAUNCH_REMAP (1u << 10)

/* Remap component selectors. */
#define REMAP_CONST_A 4u
#define REMAP_CONST_B 5u
#define REMAP_NO_WRITE 6u
#define REMAP_MAX_COMPONENTS 4u

#define BLOCK_HEIGHT_LOG2(reg) (((reg) >> 4) & 0xFu)
#define ORIGIN_X(reg) ((reg) & 0xFFFFu)
#define ORIGIN_Y(reg) ((reg) >> 16)
#define GOB_RUN 16u
#define SEMAPHORE_FOUR_WORD_BYTES 16u

void gpu_channel_init(Gpu_Channel *channel) { memset(channel, 0, sizeof(*channel)); }

static uint64_t addr40(uint32_t upper, uint32_t lower) { return ((uint64_t)(upper & 0xFFu) << 32) | lower; }

/* ------------------------------------------------------------------ */
/* DMA engine.                                                         */
/* ------------------------------------------------------------------ */

typedef struct Dma_Surface {
  uint64_t base;
  bool pitch_linear;
  uint32_t pitch;        /* pitch layout */
  uint32_t width_bytes;  /* block-linear layout */
  uint32_t block_height_log2;
  uint32_t origin_x;     /* bytes */
  uint32_t origin_y;
} Dma_Surface;

/* Moves `bytes` of line `y` of `s` from/to `buffer`. Block-linear lines
 * go in runs that stay inside one 16-byte GOB span. */
static bool surface_line(Gpu_Channel *ch, const Gpu_Memory *mem, const Dma_Surface *s, uint32_t y, uint8_t *buffer,
                         uint32_t bytes, bool write) {
  if (s->pitch_linear) {
    const uint64_t va = s->base + (uint64_t)y * s->pitch;
    return write ? mem->write(mem->user, va, buffer, bytes) : mem->read(mem->user, va, buffer, bytes);
  }
  (void)ch;
  for (uint32_t done = 0; done < bytes;) {
    const uint32_t x = s->origin_x + done;
    const uint32_t run = GOB_RUN - x % GOB_RUN < bytes - done ? GOB_RUN - x % GOB_RUN : bytes - done;
    const uint64_t va = s->base + block_linear_offset(x, s->origin_y + y, s->width_bytes, s->block_height_log2);
    const bool ok = write ? mem->write(mem->user, va, buffer + done, run) : mem->read(mem->user, va, buffer + done, run);
    if (!ok) return false;
    done += run;
  }
  return true;
}

static Dma_Surface surface_of(const Gpu_Channel *ch, bool source, uint32_t launch, uint32_t element) {
  const uint32_t *r = ch->dma;
  Dma_Surface s;
  memset(&s, 0, sizeof(s));
  if (source) {
    s.base = addr40(r[DMA_OFFSET_IN_UPPER], r[DMA_OFFSET_IN_LOWER]);
    s.pitch_linear = (launch & LAUNCH_SRC_PITCH) != 0;
    s.pitch = r[DMA_PITCH_IN];
    s.width_bytes = r[DMA_SRC_WIDTH] * element;
    s.block_height_log2 = BLOCK_HEIGHT_LOG2(r[DMA_SRC_BLOCK_SIZE]);
    s.origin_x = ORIGIN_X(r[DMA_SRC_ORIGIN]) * element;
    s.origin_y = ORIGIN_Y(r[DMA_SRC_ORIGIN]);
  } else {
    s.base = addr40(r[DMA_OFFSET_OUT_UPPER], r[DMA_OFFSET_OUT_LOWER]);
    s.pitch_linear = (launch & LAUNCH_DST_PITCH) != 0;
    s.pitch = r[DMA_PITCH_OUT];
    s.width_bytes = r[DMA_DST_WIDTH] * element;
    s.block_height_log2 = BLOCK_HEIGHT_LOG2(r[DMA_DST_BLOCK_SIZE]);
    s.origin_x = ORIGIN_X(r[DMA_DST_ORIGIN]) * element;
    s.origin_y = ORIGIN_Y(r[DMA_DST_ORIGIN]);
  }
  return s;
}

/* Rebuilds each element from source components / constants. */
static void remap_line(const Gpu_Channel *ch, const uint8_t *in, uint8_t *out, uint32_t elements) {
  const uint32_t rc = ch->dma[DMA_REMAP_COMPONENTS];
  const uint32_t comp = ((rc >> 16) & 3u) + 1u;
  const uint32_t src_n = ((rc >> 20) & 3u) + 1u, dst_n = ((rc >> 24) & 3u) + 1u;
  for (uint32_t e = 0; e < elements; e++) {
    const uint8_t *src = in + (uint64_t)e * comp * src_n;
    uint8_t *dst = out + (uint64_t)e * comp * dst_n;
    for (uint32_t i = 0; i < dst_n && i < REMAP_MAX_COMPONENTS; i++) {
      const uint32_t select = (rc >> (4u * i)) & 7u;
      if (select < src_n) memcpy(dst + i * comp, src + select * comp, comp);
      else if (select == REMAP_CONST_A) memcpy(dst + i * comp, &ch->dma[DMA_REMAP_CONST_A], comp);
      else if (select == REMAP_CONST_B) memcpy(dst + i * comp, &ch->dma[DMA_REMAP_CONST_B], comp);
      /* NO_WRITE and out-of-range selectors leave the byte as it was. */
    }
  }
}

static void dma_semaphore(Gpu_Channel *ch, const Gpu_Memory *mem, uint32_t launch) {
  const uint32_t type = (launch >> LAUNCH_SEMAPHORE_SHIFT) & LAUNCH_SEMAPHORE_MASK;
  if (type != LAUNCH_SEMAPHORE_ONE_WORD && type != LAUNCH_SEMAPHORE_FOUR_WORD) return;
  const uint64_t va = addr40(ch->dma[DMA_SEMAPHORE_A], ch->dma[DMA_SEMAPHORE_B]);
  uint8_t release[SEMAPHORE_FOUR_WORD_BYTES];
  memset(release, 0, sizeof(release)); /* payload, reserved, timestamp 0 */
  memcpy(release, &ch->dma[DMA_SEMAPHORE_PAYLOAD], 4);
  if (!mem->write(mem->user, va, release, type == LAUNCH_SEMAPHORE_ONE_WORD ? 4u : sizeof(release))) ch->faults++;
}

static void dma_launch(Gpu_Channel *ch, const Gpu_Memory *mem, uint32_t launch) {
  if (mem->renderer) {
    /* The copy may read what the 3D engine drew, or overwrite what it
     * cached (textures, render targets). */
    raster3d_flush(mem->renderer, mem);
    raster3d_begin_submission(mem->renderer);
  }
  if (launch & LAUNCH_TRANSFER_MASK) {
    const bool remap = (launch & LAUNCH_REMAP) != 0;
    const uint32_t rc = ch->dma[DMA_REMAP_COMPONENTS];
    const uint32_t comp = ((rc >> 16) & 3u) + 1u;
    const uint32_t src_elem = remap ? comp * (((rc >> 20) & 3u) + 1u) : 1u;
    const uint32_t dst_elem = remap ? comp * (((rc >> 24) & 3u) + 1u) : 1u;
    const uint32_t length = ch->dma[DMA_LINE_LENGTH_IN];
    const uint32_t lines = (launch & LAUNCH_MULTI_LINE) ? ch->dma[DMA_LINE_COUNT] : 1u;
    const Dma_Surface src = surface_of(ch, true, launch, src_elem);
    const Dma_Surface dst = surface_of(ch, false, launch, dst_elem);
    const uint64_t src_bytes = (uint64_t)length * src_elem, dst_bytes = (uint64_t)length * dst_elem;
    if (src.pitch_linear && dst.pitch_linear && !remap && lines == 1u) {
      /* 1D copies (buffer to buffer) may exceed one staging line. */
      for (uint64_t done = 0; done < src_bytes;) {
        const uint32_t n = (uint32_t)(src_bytes - done < GPU_LINE_BYTES ? src_bytes - done : GPU_LINE_BYTES);
        if (!mem->read(mem->user, src.base + done, ch->line, n) || !mem->write(mem->user, dst.base + done, ch->line, n)) {
          ch->faults++;
          break;
        }
        done += n;
      }
    } else if (src_bytes > GPU_LINE_BYTES || dst_bytes > GPU_LINE_BYTES) {
      ch->faults++;
    } else {
      for (uint32_t y = 0; y < lines; y++) {
        if (!surface_line(ch, mem, &src, y, ch->line, (uint32_t)src_bytes, false)) {
          ch->faults++;
          break;
        }
        uint8_t *out = ch->line;
        if (remap) {
          /* NO_WRITE components keep the destination's bytes. */
          if (!surface_line(ch, mem, &dst, y, ch->line_out, (uint32_t)dst_bytes, false)) memset(ch->line_out, 0, dst_bytes);
          remap_line(ch, ch->line, ch->line_out, length);
          out = ch->line_out;
        }
        if (!surface_line(ch, mem, &dst, y, out, (uint32_t)dst_bytes, true)) {
          ch->faults++;
          break;
        }
      }
    }
    ch->dma_copies++;
  }
  dma_semaphore(ch, mem, launch);
}

/* ------------------------------------------------------------------ */
/* Methods.                                                            */
/* ------------------------------------------------------------------ */

static void host_method(Gpu_Channel *ch, const Gpu_Memory *mem, uint32_t subchannel, uint32_t method, uint32_t data) {
  ch->host[method] = data;
  switch (method) {
  case HOST_BIND:
    ch->subchannel_class[subchannel] = data & CLASS_MASK;
    break;
  case HOST_SEMAPHORE_EXECUTE:
    if ((data & SEMAPHORE_OP_MASK) == SEMAPHORE_OP_RELEASE) {
      const uint64_t va = addr40(ch->host[HOST_SEMAPHORE_HI], ch->host[HOST_SEMAPHORE_LO]);
      uint8_t release[SEMAPHORE_FOUR_WORD_BYTES];
      memset(release, 0, sizeof(release));
      memcpy(release, &ch->host[HOST_SEMAPHORE_PAYLOAD], 4);
      if (!mem->write(mem->user, va, release, (data & SEMAPHORE_RELEASE_4BYTE) ? 4u : sizeof(release))) ch->faults++;
    }
    break; /* acquires are satisfied: work completes in order */
  case HOST_SYNCPOINT_OP:
    if ((data & SYNCPOINT_OP_INCREMENT) && mem->syncpoint_increment) mem->syncpoint_increment(mem->user, SYNCPOINT_ID(data));
    break;
  default:
    break;
  }
}

/* REPORT_SEMAPHORE: releases (and counter reports, whose counters all
 * read 0 here) write the payload, as one word or {payload, 0, u64
 * timestamp 0}; acquires are satisfied. */
static void report_semaphore(Gpu_Channel *ch, const Gpu_Memory *mem, uint32_t operation) {
  const uint32_t op = operation & M3D_REPORT_OPERATION_MASK;
  if (op != M3D_REPORT_RELEASE && op != M3D_REPORT_COUNTER) return;
  const uint64_t va = addr40(ch->engine3d[M3D_REPORT_SEMAPHORE_A], ch->engine3d[M3D_REPORT_SEMAPHORE_B]);
  uint8_t release[SEMAPHORE_FOUR_WORD_BYTES];
  memset(release, 0, sizeof(release));
  if (op == M3D_REPORT_RELEASE) memcpy(release, &ch->engine3d[M3D_REPORT_SEMAPHORE_C], 4);
  if (!mem->write(mem->user, va, release, (operation & M3D_REPORT_ONE_WORD) ? 4u : sizeof(release))) ch->faults++;
}

/* ------------------------------------------------------------------ */
/* Macro Method Expander.                                              */
/* ------------------------------------------------------------------ */

#define MME_REGS 8u
#define MME_STEP_LIMIT 0x100000u
#define MME_OP_ALU 0u
#define MME_OP_ADD_IMMEDIATE 1u
#define MME_OP_MERGE 2u
#define MME_OP_BFE_LSL_IMMEDIATE 3u
#define MME_OP_BFE_LSL_REGISTER 4u
#define MME_OP_STATE 5u
#define MME_OP_BRANCH 7u
#define MME_METHOD_ADDRESS_MASK 0xFFFu
#define MME_METHOD_INCREMENT_SHIFT 12u
#define MME_METHOD_INCREMENT_MASK 0x3Fu

typedef struct Mme_Run {
  uint32_t r[MME_REGS]; /* r[0] reads as zero */
  uint32_t carry;
  uint32_t method;      /* word address of the next emit */
  uint32_t increment;
  bool has_method;
  const uint32_t *params;
  uint32_t param_count, param_next;
} Mme_Run;

static void engine3d_method(Gpu_Channel *ch, const Gpu_Memory *mem, uint32_t method, uint32_t data);

static uint32_t mme_fetch(Mme_Run *m) { return m->param_next < m->param_count ? m->params[m->param_next++] : 0u; }

static void mme_store(Mme_Run *m, uint32_t reg, uint32_t value) {
  if (reg) m->r[reg] = value;
}

static uint32_t mme_mask(uint32_t size) { return size >= 32u ? 0xFFFFFFFFu : (1u << size) - 1u; }

static uint32_t mme_bfe_lsl(uint32_t value, uint32_t src_bit, uint32_t dst_bit, uint32_t size) {
  if (src_bit > 31u || dst_bit > 31u) return 0;
  return ((value >> src_bit) & mme_mask(size)) << dst_bit;
}

static int32_t mme_immediate(uint32_t insn) { return (int32_t)(insn >> 14) << 14 >> 14; } /* 18-bit signed */

/* The non-branch operations' result. */
static uint32_t mme_evaluate(Gpu_Channel *ch, Mme_Run *m, uint32_t insn) {
  const uint32_t x = m->r[(insn >> 11) & 7u], y = m->r[(insn >> 14) & 7u];
  const uint32_t src_bit = (insn >> 17) & 0x1Fu, size = (insn >> 22) & 0x1Fu, dst_bit = (insn >> 27) & 0x1Fu;
  switch (insn & 7u) {
  case MME_OP_ALU: {
    uint32_t result = 0;
    switch ((insn >> 17) & 0x1Fu) {
    case 0: result = x + y; m->carry = result < x; break;                 /* ADD */
    case 1: result = x + y + m->carry; m->carry = result < x; break;      /* ADDC */
    case 2: result = x - y; m->carry = result > x; break;                 /* SUB */
    case 3: result = x - y - m->carry; m->carry = result > x; break;      /* SUBB */
    case 8: result = x ^ y; break;
    case 9: result = x | y; break;
    case 10: result = x & y; break;
    case 11: result = x & ~y; break;
    case 12: result = ~(x & y); break;
    default: ch->mme_faults++; break;
    }
    return result;
  }
  case MME_OP_ADD_IMMEDIATE: return x + (uint32_t)mme_immediate(insn);
  case MME_OP_MERGE: return (x & ~(mme_mask(size) << dst_bit)) | (((y >> src_bit) & mme_mask(size)) << dst_bit);
  case MME_OP_BFE_LSL_IMMEDIATE: return mme_bfe_lsl(y, x, dst_bit, size);
  case MME_OP_BFE_LSL_REGISTER: return mme_bfe_lsl(y, src_bit, x, size);
  case MME_OP_STATE: return ch->engine3d[(x + (uint32_t)mme_immediate(insn)) & (GPU_3D_REGISTER_WORDS - 1u)];
  default: ch->mme_faults++; return 0;
  }
}

static void mme_set_method(Mme_Run *m, uint32_t value) {
  m->method = value & MME_METHOD_ADDRESS_MASK;
  m->increment = (value >> MME_METHOD_INCREMENT_SHIFT) & MME_METHOD_INCREMENT_MASK;
  m->has_method = true;
}

static void mme_emit(Gpu_Channel *ch, const Gpu_Memory *mem, Mme_Run *m, uint32_t value) {
  if (!m->has_method) return;
  engine3d_method(ch, mem, m->method, value);
  m->method = (m->method + m->increment) & MME_METHOD_ADDRESS_MASK;
}

/* One instruction; returns the branch target (or -1). */
static int32_t mme_step(Gpu_Channel *ch, const Gpu_Memory *mem, Mme_Run *m, uint32_t insn, uint32_t ip) {
  if ((insn & 7u) == MME_OP_BRANCH) {
    const uint32_t value = m->r[(insn >> 11) & 7u];
    const bool not_zero = (insn >> 4) & 1u;
    if ((value != 0) == not_zero) return (int32_t)ip + mme_immediate(insn);
    return -1;
  }
  const uint32_t result = mme_evaluate(ch, m, insn), dst = (insn >> 8) & 7u;
  switch ((insn >> 4) & 7u) {
  case 0: mme_store(m, dst, mme_fetch(m)); break;                                     /* fetch */
  case 1: mme_store(m, dst, result); break;                                           /* move */
  case 2: mme_store(m, dst, result); mme_set_method(m, result); break;                /* move, set method */
  case 3: mme_store(m, dst, mme_fetch(m)); mme_emit(ch, mem, m, result); break;       /* fetch, send */
  case 4: mme_store(m, dst, result); mme_emit(ch, mem, m, result); break;             /* move, send */
  case 5: mme_store(m, dst, mme_fetch(m)); mme_set_method(m, result); break;          /* fetch, set method */
  case 6: mme_store(m, dst, result); mme_set_method(m, result); mme_emit(ch, mem, m, mme_fetch(m)); break;
  default:                                                                            /* set method, send bits */
    mme_store(m, dst, result);
    mme_set_method(m, result);
    mme_emit(ch, mem, m, (result >> MME_METHOD_INCREMENT_SHIFT) & MME_METHOD_INCREMENT_MASK);
    break;
  }
  return -1;
}

/* Runs macro `macro` with its parameters: r1 starts as the first, the
 * rest are fetched in order. Branches have a delay slot unless marked
 * no-delay; an exit executes the next instruction before stopping. */
static void mme_run(Gpu_Channel *ch, const Gpu_Memory *mem, uint32_t macro, const uint32_t *params, uint32_t count) {
  Mme_Run m;
  memset(&m, 0, sizeof(m));
  m.params = params;
  m.param_count = count;
  m.r[1] = mme_fetch(&m);
  uint32_t ip = ch->mme_start[macro % GPU_MME_MACROS];
  int32_t pending_target = -1; /* a delayed branch, taken after the slot */
  bool exiting = false;
  ch->mme_runs++;
  for (uint32_t steps = 0; steps < MME_STEP_LIMIT; steps++) {
    if (ip >= GPU_MME_CODE_WORDS) break;
    const uint32_t insn = ch->mme_code[ip];
    const bool end = (insn >> 7) & 1u, is_branch = (insn & 7u) == MME_OP_BRANCH;
    const int32_t target = mme_step(ch, mem, &m, insn, ip);
    if (exiting) return; /* that was the exit's delay slot */
    uint32_t next = ip + 1u;
    if (pending_target >= 0) { /* this instruction was a branch's delay slot */
      next = (uint32_t)pending_target;
      pending_target = -1;
    }
    if (target >= 0) {
      if ((insn >> 5) & 1u) next = (uint32_t)target; /* no delay slot */
      else pending_target = target;
    }
    /* An exit flag on a delayed branch (taken or not) is ignored. */
    const bool delayed_branch = is_branch && !((insn >> 5) & 1u);
    if (end && !delayed_branch) exiting = true;
    ip = next;
  }
  ch->mme_faults++;
}

void gpu_channel_flush_macro(Gpu_Channel *ch, const Gpu_Memory *mem) {
  if (!ch->mme_pending) return;
  ch->mme_pending = false;
  mme_run(ch, mem, ch->mme_macro, ch->mme_params, ch->mme_param_count);
}

/* CALL_MME_MACRO(j) starts collecting, CALL_MME_DATA(j) appends. */
static void macro_method(Gpu_Channel *ch, const Gpu_Memory *mem, uint32_t method, uint32_t data) {
  const uint32_t macro = (method - M3D_MACRO_FIRST) >> 1;
  if (((method - M3D_MACRO_FIRST) & 1u) == 0) {
    gpu_channel_flush_macro(ch, mem);
    ch->mme_pending = true;
    ch->mme_macro = macro;
    ch->mme_param_count = 0;
  } else if (!ch->mme_pending || ch->mme_macro != macro) {
    return; /* data without its call */
  }
  if (ch->mme_param_count < GPU_MME_MAX_PARAMS) ch->mme_params[ch->mme_param_count++] = data;
  else ch->mme_faults++;
}

/* LOAD_CONSTANT_BUFFER(i): a word into the selected buffer at the load
 * offset, which then advances. */
static void constant_buffer_load(Gpu_Channel *ch, const Gpu_Memory *mem, uint32_t data) {
  const uint32_t offset = ch->engine3d[M3D_CB_LOAD_OFFSET];
  if (offset + 4u <= ch->engine3d[M3D_CB_SELECTOR_SIZE]) {
    const uint64_t va = addr40(ch->engine3d[M3D_CB_SELECTOR_ADDRESS_HI], ch->engine3d[M3D_CB_SELECTOR_ADDRESS_LO]);
    if (!mem->write(mem->user, va + offset, &data, sizeof(data))) ch->faults++;
  }
  ch->engine3d[M3D_CB_LOAD_OFFSET] = offset + 4u;
}

static void run_draw(Gpu_Channel *ch, const Gpu_Memory *mem, Raster3d_Draw_Kind kind, uint32_t topology,
                     uint32_t first, uint32_t count, uint32_t index_size) {
  ch->draws++;
  if (!mem->renderer) return;
  Raster3d_Draw draw;
  memset(&draw, 0, sizeof(draw));
  draw.kind = kind;
  draw.topology = topology;
  draw.first = first;
  draw.count = count;
  draw.index_size = index_size;
  draw.instance = ch->draw_instance;
  draw.inline_indices = ch->inline_indices;
  raster3d_draw(mem->renderer, ch->engine3d, &ch->bindings, mem, &draw);
}

static void inline_index(Gpu_Channel *ch, uint32_t index) {
  if (ch->inline_count < GPU_INLINE_INDICES) ch->inline_indices[ch->inline_count++] = index;
}

/* Clears, draws and the draw-time state that is not plain registers.
 * Returns true when `method` was one of them. */
static bool draw_method(Gpu_Channel *ch, const Gpu_Memory *mem, uint32_t method, uint32_t data) {
  if (method >= M3D_BIND_GROUP_CB && method < M3D_BIND_GROUP_CB + RASTER_BIND_GROUPS * M3D_BIND_GROUP_STRIDE &&
      (method - M3D_BIND_GROUP_CB) % M3D_BIND_GROUP_STRIDE == 0) {
    const uint32_t group = (method - M3D_BIND_GROUP_CB) / M3D_BIND_GROUP_STRIDE;
    const uint32_t slot = (data >> 4) & 0x1Fu;
    if (slot < SM_CBUF_SLOTS) {
      const bool valid = (data & 1u) != 0;
      ch->bindings.address[group][slot] =
          valid ? addr40(ch->engine3d[M3D_CB_SELECTOR_ADDRESS_HI], ch->engine3d[M3D_CB_SELECTOR_ADDRESS_LO]) : 0u;
      ch->bindings.size[group][slot] = valid ? ch->engine3d[M3D_CB_SELECTOR_SIZE] : 0u;
    }
    return true;
  }
  switch (method) {
  case M3D_BEGIN:
    ch->draw_topology = M3D_BEGIN_TOPOLOGY(data);
    if (M3D_BEGIN_INSTANCE(data) == 0) ch->draw_instance = 0;
    else if (M3D_BEGIN_INSTANCE(data) == 1) ch->draw_instance++;
    ch->inline_count = 0;
    return true;
  case M3D_END:
    if (ch->inline_count) run_draw(ch, mem, RASTER_DRAW_INLINE, ch->draw_topology, 0, ch->inline_count, 4);
    ch->inline_count = 0;
    return true;
  case M3D_DRAW_INLINE_INDEX:
    inline_index(ch, data);
    return true;
  case M3D_DRAW_INLINE_INDEX2X16:
    inline_index(ch, data & 0xFFFFu);
    inline_index(ch, data >> 16);
    return true;
  case M3D_DRAW_VERTEX_ARRAY:
    run_draw(ch, mem, RASTER_DRAW_ARRAYS, ch->draw_topology, ch->engine3d[M3D_VERTEX_ARRAY_START], data, 0);
    return true;
  case M3D_DRAW_VERTEX_ARRAY_FIRST:
  case M3D_DRAW_VERTEX_ARRAY_SUBSEQUENT:
    ch->draw_instance = method == M3D_DRAW_VERTEX_ARRAY_FIRST ? 0u : ch->draw_instance + 1u;
    run_draw(ch, mem, RASTER_DRAW_ARRAYS, M3D_COMPACT_TOPOLOGY(data), M3D_COMPACT_FIRST(data), M3D_COMPACT_COUNT(data), 0);
    return true;
  case M3D_DRAW_INDEX_BUFFER:
    run_draw(ch, mem, RASTER_DRAW_INDEXED, ch->draw_topology, ch->engine3d[M3D_INDEX_BUFFER_F], data,
             1u << (ch->engine3d[M3D_INDEX_BUFFER_E] & 3u));
    return true;
  case M3D_CLEAR_SURFACE:
    if (mem->renderer) raster3d_clear(mem->renderer, ch->engine3d, mem, data);
    return true;
  default:
    break;
  }
  if (method >= M3D_DRAW_INDEX32_FIRST && method <= M3D_DRAW_INDEX8_SUBSEQUENT) {
    const uint32_t k = method - M3D_DRAW_INDEX32_FIRST; /* 0-2 first (32/16/8), 3-5 subsequent */
    ch->draw_instance = k < 3u ? 0u : ch->draw_instance + 1u;
    static const uint32_t sizes[3] = {4, 2, 1};
    run_draw(ch, mem, RASTER_DRAW_INDEXED, M3D_COMPACT_TOPOLOGY(data), M3D_COMPACT_FIRST(data), M3D_COMPACT_COUNT(data),
             sizes[k % 3u]);
    return true;
  }
  return false;
}

static void engine3d_method(Gpu_Channel *ch, const Gpu_Memory *mem, uint32_t method, uint32_t data) {
  method &= GPU_3D_REGISTER_WORDS - 1u;
  if (method >= M3D_MACRO_FIRST) {
    macro_method(ch, mem, method, data);
    return;
  }
  if (method >= M3D_CB_LOAD_FIRST && method <= M3D_CB_LOAD_LAST) {
    constant_buffer_load(ch, mem, data);
    return;
  }
  if (method == M3D_LOAD_MME_INSTRUCTION_RAM) {
    const uint32_t at = ch->engine3d[M3D_LOAD_MME_INSTRUCTION_RAM_POINTER]++;
    if (at < GPU_MME_CODE_WORDS) ch->mme_code[at] = data;
    return;
  }
  if (method == M3D_LOAD_MME_START_ADDRESS_RAM) {
    const uint32_t at = ch->engine3d[M3D_LOAD_MME_START_ADDRESS_RAM_POINTER]++;
    if (at < GPU_MME_MACROS) ch->mme_start[at] = data;
    return;
  }
  ch->engine3d[method] = data;
  if (draw_method(ch, mem, method, data)) return;
  if (method == M3D_SYNCPT_ACTION) {
    if ((data & M3D_SYNCPT_INCREMENT) && mem->syncpoint_increment) mem->syncpoint_increment(mem->user, M3D_SYNCPT_ID(data));
  } else if (method == M3D_REPORT_SEMAPHORE_D) {
    report_semaphore(ch, mem, data);
  } else {
    ch->ignored_methods++; /* rendering state: the GPU worker's, later */
  }
}

void gpu_channel_method(Gpu_Channel *ch, const Gpu_Memory *mem, uint32_t subchannel, uint32_t method, uint32_t data) {
  ch->methods++;
  subchannel &= GPU_SUBCHANNELS - 1u;
  if (method < HOST_METHODS) {
    host_method(ch, mem, subchannel, method, data);
    return;
  }
  if (ch->subchannel_class[subchannel] == GPU_CLASS_DMA && method < GPU_DMA_REGISTER_WORDS) {
    ch->dma[method] = data;
    if (method == DMA_LAUNCH) dma_launch(ch, mem, data);
    return;
  }
  if (ch->subchannel_class[subchannel] == GPU_CLASS_3D && method < GPU_3D_REGISTER_WORDS) {
    /* Any method that is not this call's data ends the call. */
    if (ch->mme_pending && method != M3D_MACRO_FIRST + 2u * ch->mme_macro + 1u) gpu_channel_flush_macro(ch, mem);
    engine3d_method(ch, mem, method, data);
    return;
  }
  ch->ignored_methods++;
}

/* ------------------------------------------------------------------ */
/* Pushbuffer decode.                                                  */
/* ------------------------------------------------------------------ */

typedef struct Decoder {
  uint32_t method;
  uint32_t subchannel;
  uint32_t remaining;
  uint32_t op;
} Decoder;

static void decode_word(Gpu_Channel *ch, const Gpu_Memory *mem, Decoder *d, uint32_t word) {
  if (d->remaining) {
    gpu_channel_method(ch, mem, d->subchannel, d->method, word);
    d->remaining--;
    if (d->op == OP_INCREMENTING) d->method++;
    else if (d->op == OP_INCREMENT_ONCE) d->op = OP_NON_INCREMENTING, d->method++;
    return;
  }
  const uint32_t op = MH_OP(word);
  d->method = MH_METHOD(word);
  d->subchannel = MH_SUBCHANNEL(word);
  if (op == OP_IMMEDIATE) {
    gpu_channel_method(ch, mem, d->subchannel, d->method, MH_COUNT(word));
    return;
  }
  if (op == OP_INCREMENTING || op == OP_NON_INCREMENTING || op == OP_INCREMENT_ONCE) {
    d->op = op;
    d->remaining = MH_COUNT(word);
  }
  /* Anything else (including 0: a NOP word) carries no data. */
}

void gpu_channel_submit(Gpu_Channel *ch, const Gpu_Memory *mem, const uint64_t *entries, uint32_t count) {
  if (mem->renderer) raster3d_begin_submission(mem->renderer);
  for (uint32_t i = 0; i < count; i++) {
    const uint64_t va = entries[i] & GP_VA_MASK;
    const uint32_t words = (uint32_t)((entries[i] >> GP_LENGTH_SHIFT) & GP_LENGTH_MASK);
    Decoder d;
    memset(&d, 0, sizeof(d));
    for (uint32_t at = 0; at < words;) {
      const uint32_t n = words - at < GPU_FETCH_WORDS ? words - at : GPU_FETCH_WORDS;
      if (!mem->read(mem->user, va + (uint64_t)at * 4u, ch->fetch, (uint64_t)n * 4u)) {
        ch->faults++;
        break;
      }
      for (uint32_t w = 0; w < n; w++) decode_word(ch, mem, &d, ch->fetch[w]);
      at += n;
    }
  }
  gpu_channel_flush_macro(ch, mem);
  if (mem->renderer) raster3d_flush(mem->renderer, mem);
}
