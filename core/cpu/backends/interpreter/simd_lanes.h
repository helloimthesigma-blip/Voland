/**
 * Lane access and saturation helpers shared by the Advanced SIMD
 * interpreter files. Interpreter-internal.
 *
 * A vector is two little-endian 64-bit halves; element i of size esize
 * (8/16/32/64 bits) occupies bits [i*esize, (i+1)*esize). Writing a
 * 64-bit (Q = 0) result zeroes the upper half, as the architecture does.
 */
#ifndef SWITCH_CPU_BACKENDS_INTERPRETER_SIMD_LANES_H
#define SWITCH_CPU_BACKENDS_INTERPRETER_SIMD_LANES_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu/backends/interpreter/interp_internal.h"
#include "cpu/backends/interpreter/softfloat.h"

typedef struct Vec {
  uint64_t d[2];
} Vec;

static inline Vec vread(const Interp_State *s, uint32_t r) { return (Vec){{s->v[r].lo, s->v[r].hi}}; }

static inline void vwrite(Interp_State *s, uint32_t r, Vec v, bool q) {
  s->v[r].lo = v.d[0];
  s->v[r].hi = q ? v.d[1] : 0;
}

/* Writes one 64-bit half ("part" 0 = low, 1 = high) and keeps the other
 * (XTN2-style); part 0 zeroes the high half (XTN-style, Q = 0). */
static inline void vwrite_part(Interp_State *s, uint32_t r, uint64_t value, unsigned part) {
  if (part) {
    s->v[r].hi = value;
  } else {
    s->v[r].lo = value;
    s->v[r].hi = 0;
  }
}

static inline uint64_t lane_mask(unsigned esize) { return esize >= 64u ? ~(uint64_t)0 : (((uint64_t)1 << esize) - 1u); }

static inline uint64_t ue(const Vec *v, unsigned index, unsigned esize) {
  const unsigned offset = index * esize;
  return (v->d[offset >> 6] >> (offset & 63u)) & lane_mask(esize);
}

static inline int64_t se(const Vec *v, unsigned index, unsigned esize) {
  return sign_extend(ue(v, index, esize), esize);
}

static inline void put(Vec *v, unsigned index, unsigned esize, uint64_t value) {
  const unsigned offset = index * esize;
  const uint64_t m = lane_mask(esize) << (offset & 63u);
  v->d[offset >> 6] = (v->d[offset >> 6] & ~m) | ((value << (offset & 63u)) & m);
}

static inline void set_qc(Interp_State *s) { s->fpsr |= FPSR_QC; }

/* Saturate a signed value already known to fit int64 (esize < 64). */
static inline uint64_t sat_s(Interp_State *s, int64_t value, unsigned esize) {
  const int64_t max = (int64_t)(lane_mask(esize) >> 1);
  const int64_t min = -max - 1;
  if (value > max) { set_qc(s); return (uint64_t)max & lane_mask(esize); }
  if (value < min) { set_qc(s); return (uint64_t)min & lane_mask(esize); }
  return (uint64_t)value & lane_mask(esize);
}

/* Saturate a signed int64 to an unsigned lane (esize < 64). */
static inline uint64_t sat_u_from_s(Interp_State *s, int64_t value, unsigned esize) {
  if (value < 0) { set_qc(s); return 0; }
  if ((uint64_t)value > lane_mask(esize)) { set_qc(s); return lane_mask(esize); }
  return (uint64_t)value;
}

/* Saturate an unsigned uint64 to an unsigned lane (esize < 64). */
static inline uint64_t sat_u(Interp_State *s, uint64_t value, unsigned esize) {
  if (value > lane_mask(esize)) { set_qc(s); return lane_mask(esize); }
  return value;
}

/* 64-bit lane saturating add/sub (no wider type available portably). */
static inline uint64_t sqadd64(Interp_State *s, uint64_t a, uint64_t b) {
  const uint64_t r = a + b;
  if (((a ^ r) & (b ^ r)) >> 63) { set_qc(s); return (a >> 63) ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull; }
  return r;
}
static inline uint64_t sqsub64(Interp_State *s, uint64_t a, uint64_t b) {
  const uint64_t r = a - b;
  if (((a ^ b) & (a ^ r)) >> 63) { set_qc(s); return (a >> 63) ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull; }
  return r;
}
static inline uint64_t uqadd64(Interp_State *s, uint64_t a, uint64_t b) {
  const uint64_t r = a + b;
  if (r < a) { set_qc(s); return ~(uint64_t)0; }
  return r;
}
static inline uint64_t uqsub64(Interp_State *s, uint64_t a, uint64_t b) {
  if (b > a) { set_qc(s); return 0; }
  return a - b;
}

static inline FP_Env simd_env(Interp_State *s) { return (FP_Env){s->fpcr, &s->fpsr}; }

#endif /* SWITCH_CPU_BACKENDS_INTERPRETER_SIMD_LANES_H */
