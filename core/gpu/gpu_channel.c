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

/* B197 (word addresses). */
#define M3D_SYNCPT_ACTION 0xB2u
#define M3D_REPORT_SEMAPHORE_A 0x6C0u
#define M3D_REPORT_SEMAPHORE_B 0x6C1u
#define M3D_REPORT_SEMAPHORE_C 0x6C2u
#define M3D_REPORT_SEMAPHORE_D 0x6C3u
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

static void engine3d_method(Gpu_Channel *ch, const Gpu_Memory *mem, uint32_t method, uint32_t data) {
  ch->engine3d[method] = data;
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
}
