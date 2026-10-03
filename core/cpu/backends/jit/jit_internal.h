/**
 * JIT internals (docs/JIT.md). The compiler turns one guest block - a
 * straight run of A64 instructions from an executable, non-writable page,
 * ending at a branch - into one WebAssembly function
 *
 *   (func (param $state i64) (result i32))
 *
 * over the shared memory64 the core lives in. Guest registers live in wasm
 * locals inside the function; every instruction the compiler does not
 * inline (and every memory access the inline softmmu walk cannot finish)
 * calls back into the interpreter for exactly that instruction, so the
 * result is always the interpreter's.
 *
 * Return values: JIT_BLOCK_CONTINUE - the state in memory is complete and
 * the run goes on from regs.pc; JIT_BLOCK_STOP - the run ends with
 * Jit_State.exit_reason (an SVC, a fault, an undefined instruction ...).
 */
#ifndef SWITCH_CPU_BACKENDS_JIT_JIT_INTERNAL_H
#define SWITCH_CPU_BACKENDS_JIT_JIT_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu/backends/interpreter/interp_internal.h"

#define JIT_BLOCK_CONTINUE 0u
#define JIT_BLOCK_STOP 1u

/* Most instructions in one compiled block; also the budget a block needs
 * before it may be entered (one cycle per instruction, exactly). */
#define JIT_MAX_BLOCK_INSNS 128u

/* Per guest thread. Interp_State comes first: every interpreter entry
 * point takes the same pointer. */
typedef struct Jit_State {
  Interp_State interp;
  CPU_ExitReason exit_reason; /* valid after a block returned JIT_BLOCK_STOP */
} Jit_State;

typedef uint32_t (*Jit_Block_Fn)(Jit_State *state);

/* The interpreter fallback a block imports: runs `insn` at regs.pc with
 * full retirement (cycles, SVC/undefined/breakpoint handlers). Returns
 * JIT_BLOCK_CONTINUE to keep going in the block, JIT_BLOCK_STOP to end
 * the run, or JIT_HELPER_LEAVE when the block must return to the
 * dispatcher with the state complete (code may have changed). */
#define JIT_HELPER_LEAVE 2u
uint32_t jit_helper_interpret(Jit_State *state, uint32_t insn);

/* ------------------------------------------------------------------ */
/* Compiler (jit_compile.c).                                           */
/* ------------------------------------------------------------------ */

typedef struct Jit_Compiled {
  uint32_t instructions; /* guest instructions the block covers */
  uint32_t module_bytes;
} Jit_Compiled;

/* Compiles the block at `pc` (whose instruction words start at `code`,
 * `available` of them readable) into a complete wasm module in `out`
 * (capacity `capacity`). The module imports env.memory (memory64, shared,
 * `memory_pages` pages) and env.interpret (jit_helper_interpret), and
 * exports the block function as "b". Returns false if nothing could be
 * compiled (buffer too small). */
bool jit_compile_block(uint64_t pc, const uint32_t *code, uint32_t available, uint64_t memory_pages, uint8_t *out,
                       uint32_t capacity, Jit_Compiled *result);

/* Tunables and statistics (jit.c). */
void jit_set_hot_threshold(uint32_t executions);

typedef struct Jit_Stats {
  uint64_t blocks_compiled;
  uint64_t compile_failures;
  uint64_t block_entries;   /* compiled-block calls */
  uint64_t interpreted_blocks;
  uint64_t module_bytes;
} Jit_Stats;
const Jit_Stats *jit_stats(void);

#endif /* SWITCH_CPU_BACKENDS_JIT_JIT_INTERNAL_H */
