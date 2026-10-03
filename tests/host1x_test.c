/**
 * Host1x command buffers (video/host1x.h) and the multimedia IOVA table
 * (hle/services/nvdrv/nvdec.h): every opcode's register writes, the
 * METHOD0/METHOD1 latch, and IOVA map/translate/unmap.
 */
#define CHECK_NAME "host1x_test"
#include "check.h"

#include "hle/services/nvdrv/nvdec.h"
#include "video/host1x.h"

#include <string.h>

#define MAX_WRITES 64u

typedef struct Write_Log {
  uint32_t count;
  uint32_t class_id[MAX_WRITES], reg[MAX_WRITES], value[MAX_WRITES];
} Write_Log;

static void record(void *user, uint32_t class_id, uint32_t reg, uint32_t value) {
  Write_Log *log = (Write_Log *)user;
  if (log->count >= MAX_WRITES) return;
  log->class_id[log->count] = class_id;
  log->reg[log->count] = reg;
  log->value[log->count] = value;
  log->count++;
}

static uint32_t op(uint32_t opcode, uint32_t offset, uint32_t low) { return (opcode << 28) | (offset << 16) | low; }

static void test_opcodes(void) {
  const uint32_t words[] = {
      op(0, 0, (HOST1X_CLASS_NVDEC << 6) | 0x5), 0xA, 0xC, /* SETCLASS mask 0b101: regs 0 and 2 */
      op(1, 0x10, 2), 0x11, 0x22,                         /* INCR: 0x10, 0x11 */
      op(2, 0x20, 3), 1, 2, 3,                            /* NONINCR: 0x20 x3 */
      op(3, 0x30, 0x9), 0x77, 0x88,                       /* MASK 0b1001: 0x30, 0x33 */
      op(4, 0x40, 0xBEEF),                                /* IMM */
  };
  Write_Log log = {0};
  Host1x_Parser p;
  host1x_parser_init(&p);
  CHECK(host1x_parse(&p, words, sizeof(words) / 4u, record, &log));
  const uint32_t want_reg[] = {0, 2, 0x10, 0x11, 0x20, 0x20, 0x20, 0x30, 0x33, 0x40};
  const uint32_t want_value[] = {0xA, 0xC, 0x11, 0x22, 1, 2, 3, 0x77, 0x88, 0xBEEF};
  CHECK(log.count == 10u);
  for (uint32_t i = 0; i < log.count; i++) {
    CHECK(log.class_id[i] == HOST1X_CLASS_NVDEC);
    CHECK(log.reg[i] == want_reg[i] && log.value[i] == want_value[i]);
  }
  /* A truncated INCR stops at the end of the buffer. */
  const uint32_t short_words[] = {op(1, 0, 4), 1};
  memset(&log, 0, sizeof(log));
  CHECK(host1x_parse(&p, short_words, 2, record, &log) && log.count == 1u);
  /* An unknown opcode stops the parse. */
  const uint32_t bad[] = {op(6, 0, 0), op(4, 0, 1)};
  memset(&log, 0, sizeof(log));
  CHECK(!host1x_parse(&p, bad, 2, record, &log) && log.count == 0u);
}

typedef struct Method_Log {
  uint32_t count;
  uint32_t method[8], value[8];
} Method_Log;

static void on_method(void *user, uint32_t class_id, uint32_t method, uint32_t value) {
  (void)class_id;
  Method_Log *log = (Method_Log *)user;
  if (log->count < 8u) {
    log->method[log->count] = method;
    log->value[log->count++] = value;
  }
}

static void test_latch(void) {
  /* NVDEC method 0x400 = 0x1234 then 0x300 = 0: METHOD0 holds offset >> 2. */
  const uint32_t words[] = {
      op(0, 0, HOST1X_CLASS_NVDEC << 6),
      op(1, HOST1X_REG_METHOD0, 2), 0x400u >> 2, 0x1234,
      op(4, HOST1X_REG_METHOD0, 0x300u >> 2), op(4, HOST1X_REG_METHOD1, 0),
  };
  Method_Log log = {0};
  Host1x_Method_Latch latch = {0, on_method, &log};
  Host1x_Parser p;
  host1x_parser_init(&p);
  CHECK(host1x_parse(&p, words, sizeof(words) / 4u, host1x_latch_write, &latch));
  CHECK(log.count == 2u);
  CHECK(log.method[0] == 0x400u && log.value[0] == 0x1234u);
  CHECK(log.method[1] == 0x300u && log.value[1] == 0u);
}

static void test_iova(void) {
  static Mm_Iova iova;
  mm_iova_init(&iova);
  const uint32_t a = mm_iova_map(&iova, 1, 0x80001000ull, 0x3000);
  const uint32_t b = mm_iova_map(&iova, 2, 0x1234560000ull, 0x200000);
  CHECK(a && b && a != b && (a & 0xFFu) == 0 && (b & 0xFFu) == 0);
  CHECK(mm_iova_map(&iova, 1, 0x80001000ull, 0x3000) == a); /* stable */
  uint64_t gva = 0, left = 0;
  CHECK(mm_iova_translate(&iova, (uint64_t)b + 0x100, &gva, &left) && gva == 0x1234560100ull && left == 0x1FFF00u);
  CHECK(!mm_iova_translate(&iova, (uint64_t)a + 0x3000, &gva, &left)); /* past the end */
  mm_iova_unmap(&iova, 2);
  CHECK(!mm_iova_translate(&iova, b, &gva, &left));
  /* A handle whose backing changed gets a fresh mapping. */
  const uint32_t a2 = mm_iova_map(&iova, 1, 0x90000000ull, 0x3000);
  CHECK(a2 != a && mm_iova_translate(&iova, a2, &gva, &left) && gva == 0x90000000ull);
  CHECK(!mm_iova_translate(&iova, a, &gva, &left));
}

int main(void) {
  test_opcodes();
  test_latch();
  test_iova();
  printf("[host1x_test] passed\n");
  return 0;
}
