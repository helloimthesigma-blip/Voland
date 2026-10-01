/**
 * Advanced SIMD (vector and scalar forms) - interpreter-internal entry
 * points, dispatched from interp_simd_fp.c.
 */
#ifndef SWITCH_CPU_BACKENDS_INTERPRETER_INTERP_SIMD_H
#define SWITCH_CPU_BACKENDS_INTERPRETER_INTERP_SIMD_H

#include "cpu/backends/interpreter/interp_internal.h"

Interp_Status interp_simd_vector(Interp_State *s, uint32_t insn);
Interp_Status interp_simd_scalar(Interp_State *s, uint32_t insn);

#endif /* SWITCH_CPU_BACKENDS_INTERPRETER_INTERP_SIMD_H */
