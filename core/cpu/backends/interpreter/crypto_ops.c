#include "cpu/backends/interpreter/crypto_ops.h"

#include <stdbool.h>
#include <string.h>

#define BYTE_BITS 8u
#define WORD_BITS 32u
#define DWORD_BITS 64u
#define AES_BYTES 16u
#define AES_POLY 0x1Bu   /* x^8 = x^4 + x^3 + x + 1 in GF(2^8) */
#define AES_AFFINE 0x63u /* SubBytes' affine constant */

/* ---- AES ----------------------------------------------------------------- */

static uint8_t g_sbox[256], g_inv_sbox[256];
static bool g_sbox_ready;

static uint8_t gf_mul(uint8_t x, uint8_t y) {
  uint8_t r = 0;
  while (y) {
    if (y & 1u) r ^= x;
    x = (uint8_t)((x << 1) ^ ((x & 0x80u) ? AES_POLY : 0u));
    y >>= 1;
  }
  return r;
}

static uint8_t rotl8(uint8_t x, unsigned n) { return (uint8_t)((x << n) | (x >> (BYTE_BITS - n))); }

/* SubBytes from its definition: the GF(2^8) inverse, then the affine map. */
static void build_sbox(void) {
  for (unsigned x = 0; x < 256u; x++) {
    uint8_t inv = 0;
    if (x) {
      uint8_t p = (uint8_t)x; /* x^254 = x^-1 */
      inv = 1;
      for (unsigned e = 254u; e; e >>= 1) {
        if (e & 1u) inv = gf_mul(inv, p);
        p = gf_mul(p, p);
      }
    }
    const uint8_t s = (uint8_t)(inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^ rotl8(inv, 3) ^ rotl8(inv, 4) ^ AES_AFFINE);
    g_sbox[x] = s;
    g_inv_sbox[s] = (uint8_t)x;
  }
  __atomic_store_n(&g_sbox_ready, true, __ATOMIC_RELEASE); /* cores may race to build it: same bytes */
}

static void mix_column(uint8_t *c, bool inverse) {
  const uint8_t a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
  if (!inverse) {
    c[0] = (uint8_t)(gf_mul(a0, 2) ^ gf_mul(a1, 3) ^ a2 ^ a3);
    c[1] = (uint8_t)(a0 ^ gf_mul(a1, 2) ^ gf_mul(a2, 3) ^ a3);
    c[2] = (uint8_t)(a0 ^ a1 ^ gf_mul(a2, 2) ^ gf_mul(a3, 3));
    c[3] = (uint8_t)(gf_mul(a0, 3) ^ a1 ^ a2 ^ gf_mul(a3, 2));
  } else {
    c[0] = (uint8_t)(gf_mul(a0, 14) ^ gf_mul(a1, 11) ^ gf_mul(a2, 13) ^ gf_mul(a3, 9));
    c[1] = (uint8_t)(gf_mul(a0, 9) ^ gf_mul(a1, 14) ^ gf_mul(a2, 11) ^ gf_mul(a3, 13));
    c[2] = (uint8_t)(gf_mul(a0, 13) ^ gf_mul(a1, 9) ^ gf_mul(a2, 14) ^ gf_mul(a3, 11));
    c[3] = (uint8_t)(gf_mul(a0, 11) ^ gf_mul(a1, 13) ^ gf_mul(a2, 9) ^ gf_mul(a3, 14));
  }
}

/* The state is column-major: byte 4c + r is row r of column c. */
Crypto_Vec crypto_aes(Crypto_Aes_Op op, Crypto_Vec d, Crypto_Vec n) {
  if (!__atomic_load_n(&g_sbox_ready, __ATOMIC_ACQUIRE)) build_sbox();
  uint8_t s[AES_BYTES], t[AES_BYTES];
  if (op == CRYPTO_AESE || op == CRYPTO_AESD) {
    uint8_t x[AES_BYTES], y[AES_BYTES];
    memcpy(x, d.d, AES_BYTES);
    memcpy(y, n.d, AES_BYTES);
    for (unsigned i = 0; i < AES_BYTES; i++) s[i] = (uint8_t)(x[i] ^ y[i]);
    for (unsigned c = 0; c < 4u; c++)
      for (unsigned r = 0; r < 4u; r++) {
        if (op == CRYPTO_AESE) t[4u * c + r] = g_sbox[s[4u * ((c + r) & 3u) + r]]; /* ShiftRows, SubBytes */
        else t[4u * ((c + r) & 3u) + r] = g_inv_sbox[s[4u * c + r]];             /* InvShiftRows, InvSubBytes */
      }
  } else {
    memcpy(t, n.d, AES_BYTES);
    for (unsigned c = 0; c < 4u; c++) mix_column(&t[4u * c], op == CRYPTO_AESIMC);
  }
  Crypto_Vec out;
  memcpy(out.d, t, AES_BYTES);
  return out;
}

/* ---- SHA ----------------------------------------------------------------- */

static uint32_t rol32(uint32_t x, unsigned n) { return n ? (x << n) | (x >> (WORD_BITS - n)) : x; }
static uint32_t ror32(uint32_t x, unsigned n) { return n ? (x >> n) | (x << (WORD_BITS - n)) : x; }
static uint32_t w32(Crypto_Vec v, unsigned i) { return (uint32_t)(v.d[i >> 1] >> (WORD_BITS * (i & 1u))); }

static Crypto_Vec words(const uint32_t w[4]) {
  return (Crypto_Vec){{(uint64_t)w[0] | (uint64_t)w[1] << WORD_BITS, (uint64_t)w[2] | (uint64_t)w[3] << WORD_BITS}};
}

Crypto_Vec crypto_sha3(Crypto_Sha3_Op op, Crypto_Vec d, Crypto_Vec n, Crypto_Vec m) {
  uint32_t out[4];
  switch (op) {
  case CRYPTO_SHA1SU0: /* Vn<63:0>:Vd<127:64> ^ Vd ^ Vm */
    return (Crypto_Vec){{d.d[1] ^ d.d[0] ^ m.d[0], n.d[0] ^ d.d[1] ^ m.d[1]}};
  case CRYPTO_SHA1C:
  case CRYPTO_SHA1P:
  case CRYPTO_SHA1M: {
    uint32_t x[4] = {w32(d, 0), w32(d, 1), w32(d, 2), w32(d, 3)}, y = w32(n, 0);
    for (unsigned e = 0; e < 4u; e++) {
      uint32_t t;
      if (op == CRYPTO_SHA1C) t = (x[1] & x[2]) | (~x[1] & x[3]);
      else if (op == CRYPTO_SHA1P) t = x[1] ^ x[2] ^ x[3];
      else t = (x[1] & x[2]) | (x[1] & x[3]) | (x[2] & x[3]);
      y = y + rol32(x[0], 5) + t + w32(m, e);
      x[1] = rol32(x[1], 30);
      const uint32_t carry = x[3]; /* (Y:X) rotated left by 32 */
      x[3] = x[2];
      x[2] = x[1];
      x[1] = x[0];
      x[0] = y;
      y = carry;
    }
    return words(x);
  }
  case CRYPTO_SHA256H:
  case CRYPTO_SHA256H2: { /* SHA256H: X = Vd, Y = Vn; SHA256H2: X = Vn, Y = Vd */
    const bool part1 = op == CRYPTO_SHA256H;
    const Crypto_Vec vx = part1 ? d : n, vy = part1 ? n : d;
    uint32_t x[4] = {w32(vx, 0), w32(vx, 1), w32(vx, 2), w32(vx, 3)};
    uint32_t y[4] = {w32(vy, 0), w32(vy, 1), w32(vy, 2), w32(vy, 3)};
    for (unsigned e = 0; e < 4u; e++) {
      const uint32_t ch = (y[0] & y[1]) ^ (~y[0] & y[2]);
      const uint32_t maj = (x[0] & x[1]) ^ (x[0] & x[2]) ^ (x[1] & x[2]);
      const uint32_t sigma1 = ror32(y[0], 6) ^ ror32(y[0], 11) ^ ror32(y[0], 25);
      const uint32_t sigma0 = ror32(x[0], 2) ^ ror32(x[0], 13) ^ ror32(x[0], 22);
      const uint32_t t = y[3] + sigma1 + ch + w32(m, e);
      x[3] += t;
      y[3] = t + sigma0 + maj;
      /* (Y:X) rotated left by 32 bits */
      const uint32_t x3 = x[3], y3 = y[3];
      x[3] = x[2]; x[2] = x[1]; x[1] = x[0]; x[0] = y3;
      y[3] = y[2]; y[2] = y[1]; y[1] = y[0]; y[0] = x3;
    }
    return words(part1 ? x : y);
  }
  case CRYPTO_SHA256SU1:
  default: {
    const uint32_t t0[4] = {w32(n, 1), w32(n, 2), w32(n, 3), w32(m, 0)};
    for (unsigned e = 0; e < 4u; e++) {
      const uint32_t src = e < 2u ? w32(m, e + 2u) : out[e - 2u];
      const uint32_t s1 = ror32(src, 17) ^ ror32(src, 19) ^ (src >> 10);
      out[e] = s1 + w32(d, e) + t0[e];
    }
    return words(out);
  }
  }
}

Crypto_Vec crypto_sha2(Crypto_Sha2_Op op, Crypto_Vec d, Crypto_Vec n) {
  uint32_t out[4];
  switch (op) {
  case CRYPTO_SHA1H:
    return (Crypto_Vec){{rol32(w32(n, 0), 30), 0}};
  case CRYPTO_SHA1SU1: {
    const uint32_t t[4] = {w32(d, 0) ^ w32(n, 1), w32(d, 1) ^ w32(n, 2), w32(d, 2) ^ w32(n, 3), w32(d, 3)};
    out[0] = rol32(t[0], 1);
    out[1] = rol32(t[1], 1);
    out[2] = rol32(t[2], 1);
    out[3] = rol32(t[3], 1) ^ rol32(t[0], 2);
    return words(out);
  }
  case CRYPTO_SHA256SU0:
  default: {
    const uint32_t t[4] = {w32(d, 1), w32(d, 2), w32(d, 3), w32(n, 0)};
    for (unsigned e = 0; e < 4u; e++) out[e] = (ror32(t[e], 7) ^ ror32(t[e], 18) ^ (t[e] >> 3)) + w32(d, e);
    return words(out);
  }
  }
}

/* ---- polynomial multiply ------------------------------------------------- */

Crypto_Vec crypto_pmull64(uint64_t a, uint64_t b) {
  Crypto_Vec r = {{0, 0}};
  for (unsigned i = 0; i < DWORD_BITS; i++) {
    if (!((b >> i) & 1u)) continue;
    r.d[0] ^= a << i;
    if (i) r.d[1] ^= a >> (DWORD_BITS - i);
  }
  return r;
}
