/**
 * The ARMv8.0 Cryptographic Extension's arithmetic (AES rounds, SHA-1 and
 * SHA-256 schedule/hash steps, 64x64 polynomial multiply), shared by the
 * A64 interpreter and the A32 NEON interpreter, whose encodings differ but
 * whose operands mean the same thing. Interpreter-internal.
 *
 * These are CPU instructions a guest executes on its own data (TLS, hash
 * tables, save checksums); emulating them decrypts nothing of Nintendo's
 * (§1.6). Values are 128-bit vectors as two little-endian 64-bit halves.
 */
#ifndef SWITCH_CPU_BACKENDS_INTERPRETER_CRYPTO_OPS_H
#define SWITCH_CPU_BACKENDS_INTERPRETER_CRYPTO_OPS_H

#include <stdint.h>

typedef struct Crypto_Vec {
  uint64_t d[2];
} Crypto_Vec;

typedef enum Crypto_Aes_Op {
  CRYPTO_AESE = 0,  /* d ^ n, ShiftRows, SubBytes */
  CRYPTO_AESD = 1,  /* d ^ n, InvShiftRows, InvSubBytes */
  CRYPTO_AESMC = 2, /* MixColumns of n */
  CRYPTO_AESIMC = 3 /* InvMixColumns of n */
} Crypto_Aes_Op;

/* Three-register SHA forms, in A64 opcode order (bits 14:12). */
typedef enum Crypto_Sha3_Op {
  CRYPTO_SHA1C = 0,
  CRYPTO_SHA1P = 1,
  CRYPTO_SHA1M = 2,
  CRYPTO_SHA1SU0 = 3,
  CRYPTO_SHA256H = 4,
  CRYPTO_SHA256H2 = 5,
  CRYPTO_SHA256SU1 = 6
} Crypto_Sha3_Op;

/* Two-register SHA forms, in A64 opcode order (bits 16:12). */
typedef enum Crypto_Sha2_Op {
  CRYPTO_SHA1H = 0,
  CRYPTO_SHA1SU1 = 1,
  CRYPTO_SHA256SU0 = 2
} Crypto_Sha2_Op;

Crypto_Vec crypto_aes(Crypto_Aes_Op op, Crypto_Vec d, Crypto_Vec n);
/* d is the destination's old value (the hash state or schedule words). */
Crypto_Vec crypto_sha3(Crypto_Sha3_Op op, Crypto_Vec d, Crypto_Vec n, Crypto_Vec m);
Crypto_Vec crypto_sha2(Crypto_Sha2_Op op, Crypto_Vec d, Crypto_Vec n);
/* Carry-less 64x64 -> 128 multiply (PMULL/PMULL2 .1Q, VMULL.P64). */
Crypto_Vec crypto_pmull64(uint64_t a, uint64_t b);

#endif /* SWITCH_CPU_BACKENDS_INTERPRETER_CRYPTO_OPS_H */
