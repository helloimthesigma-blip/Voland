/**
 * Host1x command buffers (DESIGN §13, video decode): the word stream a
 * guest submits to /dev/nvhost-nvdec and /dev/nvhost-vic. Each word's top
 * nibble is an opcode (Tegra X1 TRM, host1x "channel command" opcodes,
 * also the Linux host1x driver's HOST1X_OPCODE_* definitions):
 *
 *   0 SETCLASS  [27:16] offset, [15:6] class, [5:0] mask  (writes like MASK)
 *   1 INCR      [27:16] offset, [15:0] count: count data words, offset++
 *   2 NONINCR   [27:16] offset, [15:0] count: count data words, same offset
 *   3 MASK      [27:16] offset, [15:0] mask: one data word per set bit
 *   4 IMM       [27:16] offset, [15:0] data
 *   5 RESTART, 6 GATHER, 0xE EXTEND: not used by the multimedia stack
 *
 * Offsets are 32-bit-word register indices of the current class. The
 * engines (NVDEC, VIC) are addressed through two registers: METHOD0
 * (0x10) holds a method's byte offset >> 2 and METHOD1 (0x11) its value
 * (the "Falcon method" interface every host1x client exposes).
 */
#ifndef SWITCH_VIDEO_HOST1X_H
#define SWITCH_VIDEO_HOST1X_H

#include <stdbool.h>
#include <stdint.h>

#define HOST1X_CLASS_HOST 0x01u
#define HOST1X_CLASS_NVDEC 0xF0u
#define HOST1X_CLASS_VIC 0x5Du
#define HOST1X_CLASS_NVJPG 0xC0u

#define HOST1X_REG_METHOD0 0x10u
#define HOST1X_REG_METHOD1 0x11u
#define HOST1X_HOST_INCR_SYNCPT 0x00u /* host class: [7:0] syncpoint id, [15:8] condition */

/* One register write in the current class. */
typedef void (*Host1x_Write)(void *user, uint32_t class_id, uint32_t reg, uint32_t value);

typedef struct Host1x_Parser {
  uint32_t class_id;
  uint64_t words;   /* diagnostics */
  uint64_t unknown; /* opcodes the parser skipped */
} Host1x_Parser;

void host1x_parser_init(Host1x_Parser *p);

/* Runs `count` words; register writes go to `write`. Returns false on an
 * opcode it cannot size (the rest of the buffer is skipped). */
bool host1x_parse(Host1x_Parser *p, const uint32_t *words, uint32_t count, Host1x_Write write, void *user);

/* Engine methods decoded from METHOD0/METHOD1 pairs. */
typedef void (*Host1x_Method)(void *user, uint32_t class_id, uint32_t method, uint32_t value);

typedef struct Host1x_Method_Latch {
  uint32_t method; /* the byte offset METHOD0 named (value << 2) */
  Host1x_Method on_method;
  void *user;
} Host1x_Method_Latch;

/* A Host1x_Write that turns METHOD0/METHOD1 pairs into on_method calls
 * (user = a Host1x_Method_Latch). */
void host1x_latch_write(void *user, uint32_t class_id, uint32_t reg, uint32_t value);

#endif /* SWITCH_VIDEO_HOST1X_H */
