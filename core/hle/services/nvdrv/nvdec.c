/**
 * Host1x multimedia engines. See nvdec.h.
 */
#include "hle/services/nvdrv/nvdec.h"

#include "common/log.h"
#include "video/host1x.h"

#include <string.h>

#define MM_METHOD_EXECUTE 0x300u
#define MM_BUFFER_SHIFT 8u
#define MM_DISCOVERY_EXECUTES 6u  /* executes whose buffers are dumped (discovery) */
#define MM_DUMP_BYTES 0x40u
#define MM_DUMP_FIRST_REG (0x400u / 4u)
#define MM_DUMP_LINE 16u

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

bool mm_iova_translate(const Mm_Iova *iova, uint64_t address, uint64_t *guest_va, uint64_t *remaining) {
  for (uint32_t i = 0; i < MM_MAX_IOVA_MAPS; i++) {
    const Mm_Iova_Map *m = &iova->maps[i];
    if (!m->handle || address < m->iova || address - m->iova >= m->size) continue;
    *guest_va = m->guest_va + (address - m->iova);
    *remaining = m->size - (address - m->iova);
    return true;
  }
  return false;
}

/* ---- Engines ------------------------------------------------------- */

void mm_engine_init(Mm_Engine *engine, uint32_t class_id) {
  memset(engine, 0, sizeof(*engine));
  engine->class_id = class_id;
}

typedef struct Submit_Ctx {
  Mm_Engine *engine;
  const Mm_Context *context;
} Submit_Ctx;

static void dump_buffer(const Mm_Context *context, uint32_t reg, uint32_t value) {
  uint64_t guest = 0, remaining = 0;
  const uint64_t address = (uint64_t)value << MM_BUFFER_SHIFT;
  if (!mm_iova_translate(context->iova, address, &guest, &remaining)) {
    log_info("[video]   reg 0x%03x = 0x%08x: no buffer", reg * 4u, value);
    return;
  }
  uint8_t bytes[MM_DUMP_BYTES];
  const uint64_t n = remaining < MM_DUMP_BYTES ? remaining : MM_DUMP_BYTES;
  if (!error_is_ok(vmm_read_block(context->vmm, guest, bytes, n))) return;
  log_info("[video]   reg 0x%03x = 0x%08x -> gva 0x%llx (%llu left)", reg * 4u, value, (unsigned long long)guest,
           (unsigned long long)remaining);
  for (uint64_t at = 0; at < n; at += MM_DUMP_LINE) {
    char line[MM_DUMP_LINE * 3u + 1u];
    for (uint32_t k = 0; k < MM_DUMP_LINE; k++) {
      static const char hex[] = "0123456789abcdef";
      line[k * 3u] = hex[bytes[at + k] >> 4];
      line[k * 3u + 1u] = hex[bytes[at + k] & 0xFu];
      line[k * 3u + 2u] = ' ';
    }
    line[MM_DUMP_LINE * 3u] = 0;
    log_info("[video]     +%03llx %s", (unsigned long long)at, line);
  }
}

static void execute(Mm_Engine *engine, const Mm_Context *context, uint32_t argument) {
  engine->executes++;
  if (engine->executes > MM_DISCOVERY_EXECUTES) return;
  log_info("[video] class 0x%x EXECUTE #%llu (arg 0x%x)", engine->class_id, (unsigned long long)engine->executes,
           argument);
  for (uint32_t r = 0; r < MM_ENGINE_REGS; r++) {
    if (!engine->regs[r] || r == MM_METHOD_EXECUTE / 4u) continue;
    if (r >= MM_DUMP_FIRST_REG) {
      dump_buffer(context, r, engine->regs[r]);
    } else {
      log_info("[video]   reg 0x%03x = 0x%08x", r * 4u, engine->regs[r]);
    }
  }
}

static void on_method(void *user, uint32_t class_id, uint32_t method, uint32_t value) {
  Submit_Ctx *ctx = (Submit_Ctx *)user;
  if (method & 0x80000000u) return; /* host-class register (syncpoint increments) */
  if (class_id != ctx->engine->class_id) {
    log_debug("[video] method 0x%x for class 0x%x on engine 0x%x", method, class_id, ctx->engine->class_id);
    return;
  }
  if (method / 4u >= MM_ENGINE_REGS) return;
  ctx->engine->regs[method / 4u] = value;
  if (method == MM_METHOD_EXECUTE) execute(ctx->engine, ctx->context, value);
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
