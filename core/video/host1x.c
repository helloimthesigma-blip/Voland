/**
 * Host1x command-buffer parser. See host1x.h.
 */
#include "video/host1x.h"

#define OPCODE(w) ((w) >> 28)
#define OP_OFFSET(w) (((w) >> 16) & 0xFFFu)
#define OP_COUNT(w) ((w) & 0xFFFFu)
#define SETCLASS_CLASS(w) (((w) >> 6) & 0x3FFu)
#define SETCLASS_MASK(w) ((w) & 0x3Fu)
#define MASK_BITS 16u

enum {
  OP_SETCLASS = 0,
  OP_INCR = 1,
  OP_NONINCR = 2,
  OP_MASK = 3,
  OP_IMM = 4,
  OP_RESTART = 5,
  OP_GATHER = 6,
  OP_SETSTRMID = 7,
  OP_SETAPPID = 8,
  OP_SETPYLD = 9,
  OP_EXTEND = 0xE,
};

void host1x_parser_init(Host1x_Parser *p) {
  p->class_id = HOST1X_CLASS_HOST;
  p->words = 0;
  p->unknown = 0;
}

static uint32_t masked_writes(Host1x_Parser *p, const uint32_t *words, uint32_t i, uint32_t count, uint32_t offset,
                              uint32_t mask, uint32_t bits, Host1x_Write write, void *user) {
  for (uint32_t b = 0; b < bits; b++) {
    if (!(mask & (1u << b))) continue;
    if (i >= count) return i;
    write(user, p->class_id, offset + b, words[i++]);
  }
  return i;
}

bool host1x_parse(Host1x_Parser *p, const uint32_t *words, uint32_t count, Host1x_Write write, void *user) {
  uint32_t i = 0;
  while (i < count) {
    const uint32_t w = words[i++];
    p->words++;
    const uint32_t offset = OP_OFFSET(w);
    switch (OPCODE(w)) {
    case OP_SETCLASS:
      p->class_id = SETCLASS_CLASS(w);
      i = masked_writes(p, words, i, count, offset, SETCLASS_MASK(w), 6u, write, user);
      break;
    case OP_INCR:
      for (uint32_t k = 0; k < OP_COUNT(w) && i < count; k++) write(user, p->class_id, offset + k, words[i++]);
      break;
    case OP_NONINCR:
      for (uint32_t k = 0; k < OP_COUNT(w) && i < count; k++) write(user, p->class_id, offset, words[i++]);
      break;
    case OP_MASK:
      i = masked_writes(p, words, i, count, offset, OP_COUNT(w), MASK_BITS, write, user);
      break;
    case OP_IMM:
      write(user, p->class_id, offset, OP_COUNT(w));
      break;
    case OP_SETSTRMID: case OP_SETAPPID: case OP_SETPYLD: case OP_EXTEND:
      p->unknown++; /* one-word opcodes with no register write */
      break;
    default:
      p->unknown++;
      return false;
    }
  }
  return true;
}

void host1x_latch_write(void *user, uint32_t class_id, uint32_t reg, uint32_t value) {
  Host1x_Method_Latch *latch = (Host1x_Method_Latch *)user;
  if (reg == HOST1X_REG_METHOD0) {
    latch->method = value << 2;
  } else if (reg == HOST1X_REG_METHOD1) {
    if (latch->on_method) latch->on_method(latch->user, class_id, latch->method, value);
  } else if (latch->on_method) {
    /* Host-class writes (syncpoint increments) pass through with the
     * register index tagged so they never collide with a method. */
    latch->on_method(latch->user, class_id, 0x80000000u | reg, value);
  }
}
