/**
 * CPU_BACKEND_JIT: Voland's own ARM64 -> WebAssembly JIT (CLAUDE.md rule
 * 10, as amended 2026-10-03; design in docs/JIT.md). Mixed mode over the
 * interpreter: the architectural state IS Interp_State, cold code runs
 * predecoded/interpreted, hot blocks are compiled to wasm functions, and
 * anything the compiler does not cover falls back to the interpreter one
 * instruction at a time. Selected with -DCPU_BACKEND=jit.
 *
 * Natively there is no wasm engine to run compiled blocks, so the native
 * backend is the interpreter plus the emitter (which tests validate); the
 * compiled path only executes under Emscripten.
 */
#ifndef SWITCH_CPU_BACKENDS_JIT_JIT_H
#define SWITCH_CPU_BACKENDS_JIT_JIT_H

#include "cpu/cpu.h"

#include <stdbool.h>
#include <stdint.h>

/* CPU_BACKEND_JIT is declared in cpu.h. */

/* Process-wide counters, for measurement (voland-cli prints them). */
typedef struct Jit_Stats {
  uint64_t blocks_compiled;    /* regions, each a wasm function */
  uint64_t region_blocks;      /* guest blocks in them */
  uint64_t compile_failures;
  uint64_t block_entries;      /* compiled-block calls */
  uint64_t interpreted_blocks; /* blocks the interpreter ran instead */
  uint64_t module_bytes;
  uint64_t generations;        /* code-generation changes seen (each retires every block) */
  uint64_t evictions;          /* compiled blocks dropped from the cache */
  uint64_t stale;              /* compiled blocks whose code changed or was unmapped */
  uint64_t revalidations;      /* compiled blocks kept across a generation change */
  uint64_t direct_simd;        /* SIMD&FP instructions compiled code ran through jit_helper_simd */
  uint64_t simd_fpcr_nonzero;  /* ...with FPCR != 0 (no native fast paths) */
  uint64_t simd_ixc_clear;     /* ...with FPCR 0 but FPSR.IXC clear */
  uint64_t last_fpcr;
  uint64_t helper_simd_fp;     /* instructions compiled code handed to the interpreter: SIMD&FP */
  uint64_t helper_memory_simd; /* ...SIMD&FP loads/stores */
  uint64_t helper_memory_exclusive; /* ...exclusive and acquire/release */
  uint64_t helper_memory;      /* ...other loads/stores (faults, LSE...) */
  uint64_t helper_system;      /* ...system, exceptions */
  uint64_t helper_other;       /* ...everything else */
} Jit_Stats;
const Jit_Stats *jit_stats(void);

/* Interpreted executions of a block before it is compiled. */
void jit_set_hot_threshold(uint32_t executions);

/* Diagnostics: count interpreter fallbacks per opcode (register fields
 * masked out) and print the commonest `top` to stderr. */
void jit_set_fallback_profile(bool enabled);
void jit_print_fallback_profile(uint32_t top);

/* Diagnostics: write every compiled module to DIR/<pc>.wasm (NULL: off). */
void jit_set_dump_directory(const char *directory);

#endif /* SWITCH_CPU_BACKENDS_JIT_JIT_H */
