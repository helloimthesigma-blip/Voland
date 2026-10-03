/**
 * A minimal WebAssembly binary writer for the JIT (docs/JIT.md): a byte
 * buffer with LEB128 encoders and the opcodes the compiler uses. Encodings
 * follow the WebAssembly core specification (binary format, section 5),
 * with the memory64 proposal's 64-bit memarg offsets and limits.
 *
 * The buffer never allocates: it writes into caller-provided storage and
 * latches `overflow` instead of writing past the end.
 */
#ifndef SWITCH_CPU_BACKENDS_JIT_JIT_WASM_H
#define SWITCH_CPU_BACKENDS_JIT_JIT_WASM_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct Wasm_Buf {
  uint8_t *data;
  uint32_t length;
  uint32_t capacity;
  bool overflow;
} Wasm_Buf;

static inline Wasm_Buf wasm_buf(uint8_t *data, uint32_t capacity) {
  Wasm_Buf b = {data, 0, capacity, false};
  return b;
}

static inline void wasm_u8(Wasm_Buf *b, uint8_t value) {
  if (b->length >= b->capacity) {
    b->overflow = true;
    return;
  }
  b->data[b->length++] = value;
}

static inline void wasm_bytes(Wasm_Buf *b, const void *bytes, uint32_t count) {
  if (b->capacity - b->length < count) {
    b->overflow = true;
    return;
  }
  memcpy(b->data + b->length, bytes, count);
  b->length += count;
}

#define WASM_LEB_PAYLOAD_BITS 7u
#define WASM_LEB_PAYLOAD_MASK 0x7Fu
#define WASM_LEB_CONTINUE 0x80u
#define WASM_LEB_SIGN_BIT 0x40u

static inline void wasm_uleb(Wasm_Buf *b, uint64_t value) {
  do {
    uint8_t byte = (uint8_t)(value & WASM_LEB_PAYLOAD_MASK);
    value >>= WASM_LEB_PAYLOAD_BITS;
    if (value) byte |= WASM_LEB_CONTINUE;
    wasm_u8(b, byte);
  } while (value);
}

static inline void wasm_sleb(Wasm_Buf *b, int64_t value) {
  for (;;) {
    const uint8_t byte = (uint8_t)(value & WASM_LEB_PAYLOAD_MASK);
    value >>= WASM_LEB_PAYLOAD_BITS; /* arithmetic: clang/gcc/emcc */
    const bool done = (value == 0 && !(byte & WASM_LEB_SIGN_BIT)) || (value == -1 && (byte & WASM_LEB_SIGN_BIT));
    wasm_u8(b, done ? byte : (uint8_t)(byte | WASM_LEB_CONTINUE));
    if (done) return;
  }
}

/* A fixed-width (5-byte) unsigned LEB, so a size can be patched in after
 * the bytes it measures are written. */
#define WASM_PATCHABLE_LEB_BYTES 5u
static inline uint32_t wasm_reserve_size(Wasm_Buf *b) {
  const uint32_t at = b->length;
  for (uint32_t i = 0; i < WASM_PATCHABLE_LEB_BYTES; i++) wasm_u8(b, 0);
  return at;
}
static inline void wasm_patch_size(Wasm_Buf *b, uint32_t at) {
  if (b->overflow) return;
  uint32_t value = b->length - at - WASM_PATCHABLE_LEB_BYTES;
  for (uint32_t i = 0; i < WASM_PATCHABLE_LEB_BYTES; i++) {
    uint8_t byte = (uint8_t)(value & WASM_LEB_PAYLOAD_MASK);
    value >>= WASM_LEB_PAYLOAD_BITS;
    if (i + 1u < WASM_PATCHABLE_LEB_BYTES) byte |= WASM_LEB_CONTINUE;
    b->data[at + i] = byte;
  }
}

/* Module structure. */
#define WASM_MAGIC "\0asm"
#define WASM_MAGIC_BYTES 4u
#define WASM_VERSION 1u
#define WASM_VERSION_BYTES 4u

enum {
  WASM_SECTION_TYPE = 1,
  WASM_SECTION_IMPORT = 2,
  WASM_SECTION_FUNCTION = 3,
  WASM_SECTION_EXPORT = 7,
  WASM_SECTION_CODE = 10,
};

enum {
  WASM_EXTERNAL_FUNCTION = 0,
  WASM_EXTERNAL_MEMORY = 2,
};

/* Limits flags: has-maximum | shared | 64-bit index (memory64). */
#define WASM_LIMITS_HAS_MAX 0x01u
#define WASM_LIMITS_SHARED 0x02u
#define WASM_LIMITS_MEMORY64 0x04u

#define WASM_PAGE_BYTES 65536ull

enum {
  WASM_TYPE_I32 = 0x7F,
  WASM_TYPE_I64 = 0x7E,
  WASM_TYPE_F32 = 0x7D,
  WASM_TYPE_F64 = 0x7C,
  WASM_TYPE_V128 = 0x7B,
  WASM_TYPE_FUNC = 0x60,
  WASM_BLOCK_VOID = 0x40,
};

enum {
  WASM_OP_UNREACHABLE = 0x00,
  WASM_OP_NOP = 0x01,
  WASM_OP_BLOCK = 0x02,
  WASM_OP_LOOP = 0x03,
  WASM_OP_IF = 0x04,
  WASM_OP_ELSE = 0x05,
  WASM_OP_END = 0x0B,
  WASM_OP_BR = 0x0C,
  WASM_OP_BR_IF = 0x0D,
  WASM_OP_BR_TABLE = 0x0E,
  WASM_OP_RETURN = 0x0F,
  WASM_OP_CALL = 0x10,
  WASM_OP_DROP = 0x1A,
  WASM_OP_SELECT = 0x1B,
  WASM_OP_LOCAL_GET = 0x20,
  WASM_OP_LOCAL_SET = 0x21,
  WASM_OP_LOCAL_TEE = 0x22,

  WASM_OP_I32_LOAD = 0x28,
  WASM_OP_I64_LOAD = 0x29,
  WASM_OP_I32_LOAD8_U = 0x2D,
  WASM_OP_I64_LOAD8_S = 0x30,
  WASM_OP_I64_LOAD8_U = 0x31,
  WASM_OP_I64_LOAD16_S = 0x32,
  WASM_OP_I64_LOAD16_U = 0x33,
  WASM_OP_I64_LOAD32_S = 0x34,
  WASM_OP_I64_LOAD32_U = 0x35,
  WASM_OP_I32_STORE = 0x36,
  WASM_OP_I64_STORE = 0x37,
  WASM_OP_I32_STORE8 = 0x3A,
  WASM_OP_I64_STORE8 = 0x3C,
  WASM_OP_I64_STORE16 = 0x3D,
  WASM_OP_I64_STORE32 = 0x3E,

  WASM_OP_I32_CONST = 0x41,
  WASM_OP_I64_CONST = 0x42,

  WASM_OP_I32_EQZ = 0x45,
  WASM_OP_I32_EQ = 0x46,
  WASM_OP_I32_NE = 0x47,
  WASM_OP_I64_EQZ = 0x50,
  WASM_OP_I64_EQ = 0x51,
  WASM_OP_I64_NE = 0x52,
  WASM_OP_I64_LT_U = 0x54,
  WASM_OP_I64_GT_U = 0x56,
  WASM_OP_I64_LE_U = 0x58,
  WASM_OP_I64_GE_U = 0x5A,

  WASM_OP_I32_CLZ = 0x67,
  WASM_OP_I32_ADD = 0x6A,
  WASM_OP_I32_SUB = 0x6B,
  WASM_OP_I32_AND = 0x71,
  WASM_OP_I32_OR = 0x72,
  WASM_OP_I32_XOR = 0x73,
  WASM_OP_I32_SHL = 0x74,
  WASM_OP_I32_SHR_S = 0x75,
  WASM_OP_I32_SHR_U = 0x76,
  WASM_OP_I32_ROTR = 0x78,

  WASM_OP_I64_CLZ = 0x79,
  WASM_OP_I64_ADD = 0x7C,
  WASM_OP_I64_SUB = 0x7D,
  WASM_OP_I64_MUL = 0x7E,
  WASM_OP_I64_DIV_S = 0x7F,
  WASM_OP_I64_DIV_U = 0x80,
  WASM_OP_I64_AND = 0x83,
  WASM_OP_I64_OR = 0x84,
  WASM_OP_I64_XOR = 0x85,
  WASM_OP_I64_SHL = 0x86,
  WASM_OP_I64_SHR_S = 0x87,
  WASM_OP_I64_SHR_U = 0x88,
  WASM_OP_I64_ROTR = 0x8A,

  WASM_OP_I32_WRAP_I64 = 0xA7,
  WASM_OP_I64_EXTEND_I32_S = 0xAC,
  WASM_OP_I64_EXTEND_I32_U = 0xAD,
  WASM_OP_I64_EXTEND8_S = 0xC2,
  WASM_OP_I64_EXTEND16_S = 0xC3,
  WASM_OP_I64_EXTEND32_S = 0xC4,
};

#endif /* SWITCH_CPU_BACKENDS_JIT_JIT_WASM_H */
