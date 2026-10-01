/**
 * IEEE 754 arithmetic with ARM semantics, in integer arithmetic only.
 *
 * Why not host floats (§3 "Known leaks, pre-rejected: SIMD semantics"):
 * x86, ARM and WebAssembly disagree on NaN propagation, the default NaN,
 * flush-to-zero, and when underflow is detected; WebAssembly cannot change
 * the rounding mode or read exception flags at all. Guest-visible FP must
 * be identical on every platform, so the core computes it bit by bit,
 * following the Arm ARM pseudocode (FPUnpack, FPRound, FPProcessNaNs, ...):
 *   - FPCR.FZ flushes denormal inputs (IDC) and outputs (UFC, no IXC);
 *   - FPCR.DN replaces every NaN result with the default NaN;
 *   - underflow is detected before rounding (tininess before rounding);
 *   - NaN operands are prioritized SNaN-first, in operand order.
 * Verified bit-exactly - results and FPSR flags - against an ARM64 CPU by
 * tests/a64_diff_test.c.
 *
 * Values are raw encodings (uint16/32/64 in a uint64_t). Formats: half
 * (conversions only - the Switch's ARMv8.0 core has no FP16 arithmetic),
 * single, double.
 */
#ifndef SWITCH_CPU_BACKENDS_INTERPRETER_SOFTFLOAT_H
#define SWITCH_CPU_BACKENDS_INTERPRETER_SOFTFLOAT_H

#include <stdbool.h>
#include <stdint.h>

/* FPCR fields. */
#define FPCR_AHP (1u << 26)
#define FPCR_DN (1u << 25)
#define FPCR_FZ (1u << 24)
#define FPCR_RMODE_SHIFT 22u
#define FPCR_RMODE_MASK 3u

/* FPSR cumulative exception bits. */
#define FPSR_IOC (1u << 0) /* invalid operation */
#define FPSR_DZC (1u << 1) /* divide by zero */
#define FPSR_OFC (1u << 2) /* overflow */
#define FPSR_UFC (1u << 3) /* underflow */
#define FPSR_IXC (1u << 4) /* inexact */
#define FPSR_IDC (1u << 7) /* input denormal */
#define FPSR_QC (1u << 27) /* saturation (integer SIMD) */

typedef enum FP_Rounding {
  FP_ROUND_NEAREST_EVEN = 0, /* RN */
  FP_ROUND_PLUS_INF = 1,     /* RP */
  FP_ROUND_MINUS_INF = 2,    /* RM */
  FP_ROUND_ZERO = 3,         /* RZ */
  FP_ROUND_TIE_AWAY = 4,     /* FRINTA / FCVTA*: not an FPCR mode */
  FP_ROUND_ODD = 5,          /* FCVTXN: von Neumann rounding */
} FP_Rounding;

typedef enum FP_Format { FP_HALF = 0, FP_SINGLE = 1, FP_DOUBLE = 2 } FP_Format;

/* Per-operation environment: the FPCR in force and where flags accumulate. */
typedef struct FP_Env {
  uint32_t fpcr;
  uint32_t *fpsr;
} FP_Env;

static inline FP_Rounding fp_env_rounding(const FP_Env *env) {
  return (FP_Rounding)((env->fpcr >> FPCR_RMODE_SHIFT) & FPCR_RMODE_MASK);
}

/* Arithmetic (single and double; `format` selects). */
uint64_t fp_add(FP_Format format, uint64_t a, uint64_t b, FP_Env *env);
uint64_t fp_sub(FP_Format format, uint64_t a, uint64_t b, FP_Env *env);
uint64_t fp_mul(FP_Format format, uint64_t a, uint64_t b, FP_Env *env);
uint64_t fp_mulx(FP_Format format, uint64_t a, uint64_t b, FP_Env *env); /* FMULX: inf*0 = ±2 */
uint64_t fp_div(FP_Format format, uint64_t a, uint64_t b, FP_Env *env);
uint64_t fp_sqrt(FP_Format format, uint64_t a, FP_Env *env);
/* addend + a * b with a single rounding (FMADD and friends negate inputs). */
uint64_t fp_mul_add(FP_Format format, uint64_t addend, uint64_t a, uint64_t b, FP_Env *env);
uint64_t fp_max(FP_Format format, uint64_t a, uint64_t b, FP_Env *env);
uint64_t fp_min(FP_Format format, uint64_t a, uint64_t b, FP_Env *env);
uint64_t fp_max_num(FP_Format format, uint64_t a, uint64_t b, FP_Env *env);
uint64_t fp_min_num(FP_Format format, uint64_t a, uint64_t b, FP_Env *env);
uint64_t fp_abs(FP_Format format, uint64_t a);
uint64_t fp_neg(FP_Format format, uint64_t a);

/* FRINT*: `exact` raises IXC when the value changes (FRINTX). */
uint64_t fp_round_int(FP_Format format, uint64_t a, FP_Rounding rounding, bool exact, FP_Env *env);

/* FCMP/FCMPE: returns NZCV (4 bits). `signal_all_nans` = FCMPE. */
uint32_t fp_compare(FP_Format format, uint64_t a, uint64_t b, bool signal_all_nans, FP_Env *env);
/* Vector compares (FCMEQ/FCMGE/FCMGT): true/false; any NaN is false.
 * EQ signals only on SNaN; GE/GT signal on any NaN. */
bool fp_compare_eq(FP_Format format, uint64_t a, uint64_t b, FP_Env *env);
bool fp_compare_ge(FP_Format format, uint64_t a, uint64_t b, FP_Env *env);
bool fp_compare_gt(FP_Format format, uint64_t a, uint64_t b, FP_Env *env);

/* FCVT between formats (rounding per FPCR; half <-> single/double too). */
uint64_t fp_convert(FP_Format to, FP_Format from, uint64_t a, FP_Env *env);

/* FCVTXN: double -> single with round-to-odd. */
uint64_t fp_convert_round_odd(uint64_t a, FP_Env *env);

/* FP -> integer (FCVT[NAPMZ][SU], fixed-point when fbits > 0): saturating,
 * NaN -> 0 with IOC, out of range -> saturate with IOC, inexact -> IXC. */
uint64_t fp_to_int(FP_Format format, uint64_t a, unsigned fbits, bool is_unsigned, unsigned int_bits,
                   FP_Rounding rounding, FP_Env *env);
/* Integer -> FP (SCVTF/UCVTF, fixed-point when fbits > 0), FPCR rounding. */
uint64_t fp_from_int(FP_Format format, uint64_t value, unsigned fbits, bool is_signed, unsigned int_bits,
                     FP_Env *env);

/* FRECPE / FRSQRTE estimates and the FRECPS / FRSQRTS Newton steps. */
uint64_t fp_recip_estimate(FP_Format format, uint64_t a, FP_Env *env);
uint64_t fp_rsqrt_estimate(FP_Format format, uint64_t a, FP_Env *env);
uint64_t fp_recip_step(FP_Format format, uint64_t a, uint64_t b, FP_Env *env);  /* 2 - a*b */
uint64_t fp_rsqrt_step(FP_Format format, uint64_t a, uint64_t b, FP_Env *env);  /* (3 - a*b) / 2 */
/* FRECPX: exponent-only reciprocal. */
uint64_t fp_recip_exponent(FP_Format format, uint64_t a, FP_Env *env);

/* Unsigned integer estimates (URECPE/URSQRTE). */
uint32_t fp_unsigned_recip_estimate(uint32_t a);
uint32_t fp_unsigned_rsqrt_estimate(uint32_t a);

/* Constants for FMOV (immediate) and friends. */
uint64_t fp_expand_imm8(FP_Format format, uint32_t imm8);

#endif /* SWITCH_CPU_BACKENDS_INTERPRETER_SOFTFLOAT_H */
