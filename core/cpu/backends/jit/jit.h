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

/* CPU_BACKEND_JIT is declared in cpu.h. The same JIT for AArch32 titles
 * (the A32 front end; state and cold path are cpu/backends/a32's): */
extern const CPU_Backend CPU_BACKEND_A32_JIT;

/* Process-wide counters, for measurement (voland-cli prints them). */
typedef struct Jit_Stats {
  uint64_t blocks_compiled;    /* regions, each a wasm function */
  uint64_t async_compiles;     /* of those requested, compiled off-thread (browser) */
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
  uint64_t codegen_ns;         /* host time generating module bytes (jit_compile_block) */
  uint64_t compile_lock_ns;    /* ...waiting for another thread's codegen */
  uint64_t sync_installs;      /* modules compiled and instantiated synchronously */
  uint64_t sync_install_ns;    /* ...and the host time that took (cores always; async is the CPU worker's) */
  uint64_t trimmed;            /* regions removed to stay under the live cap (also in evictions) */
  uint64_t inner_entries;      /* regions compiled for a PC that starts a block inside another compiled region */
  uint64_t recompiles;         /* regions compiled for an entry PC compiled before (any thread; lossy, diagnostics) */
} Jit_Stats;
const Jit_Stats *jit_stats(void);

/* Whether regions follow calls (BL/RET, predicted PLT branches). */
void jit_set_span_calls(bool enabled);

/* The JIT's version, shared by CPU_BACKEND_JIT (A64) and
 * CPU_BACKEND_A32_JIT (the UI shows it; docs/JIT.md "Versions"). */
#define JIT_VERSION "0.5.5"

/* Interpreted executions of a block before it is compiled. */
void jit_set_hot_threshold(uint32_t executions);

/* Installed regions per host thread before the least recently entered
 * are removed (0: the default, JIT_MAX_LIVE_REGIONS in jit.c). */
void jit_set_max_live_regions(uint32_t regions);

/* Diagnostics: count interpreter fallbacks per opcode (register fields
 * masked out) and print the commonest `top` to stderr. */
void jit_set_fallback_profile(bool enabled);
void jit_print_fallback_profile(uint32_t top);

/* Diagnostics: count region entries; print the `top` regions by entries
 * times entry-block length (PC, entries, instructions in the region). */
void jit_set_hot_profile(bool enabled);
void jit_print_hot_regions(uint32_t top);
/* ...and per guest thread: regions entered and translation-cache misses
 * so far (false while not counting). */
bool jit_region_counts(const CPU_State *state, uint64_t *entries, uint64_t *misses);
/* ...and why its regions left (to another region or the dispatcher). */
typedef enum {
  JIT_EXIT_BRANCH,        /* a direct branch or fall-through out of the region */
  JIT_EXIT_BUDGET,        /* an in-region block that did not fit the budget */
  JIT_EXIT_CALL,          /* BL to a callee that is a region of its own */
  JIT_EXIT_RETURN,        /* RET (A32: a return) not to an in-region return site */
  JIT_EXIT_INDIRECT,      /* BR, BLR, A32 BX/LDR PC */
  JIT_EXIT_INTERPRETER,   /* an instruction the dispatcher interprets */
  JIT_EXIT_KINDS
} Jit_Exit_Kind;
extern const char *const jit_exit_kind_names[JIT_EXIT_KINDS];
bool jit_region_exits(const CPU_State *state, uint64_t exits[JIT_EXIT_KINDS]);

/* Diagnostics: run every thread on the interpreter from now on (a JIT run
 * reaches a scene fast; this checks the JIT there - voland-cli's snapshot
 * job line "interpret"). */
void jit_set_interpret_only(bool enabled);

/* Diagnostics: write every compiled module to DIR/<pc>.wasm (NULL: off). */
void jit_set_dump_directory(const char *directory);

#endif /* SWITCH_CPU_BACKENDS_JIT_JIT_H */
