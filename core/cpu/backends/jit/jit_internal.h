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
  uint64_t cycle_budget;      /* this run()'s; compiled blocks chain while it lasts */
  uint64_t scratch[4];        /* page-crossing accesses (up to a Q-register pair) */
} Jit_State;

/* The compiled-block cache (jit.c), direct-mapped by a hash of the PC.
 * Compiled blocks read it too: a block's exit looks its successor up and
 * tail-calls it when it is compiled, current and fits the budget
 * (jit_compile.c), so the dispatcher in C only sees the misses. */
typedef struct Jit_Entry {
  uint64_t pc;
  uint64_t generation; /* the code generation it was last validated in */
  uint64_t function;   /* table index; 0 = empty */
  uint64_t code_hash;  /* of the instruction words the code depends on */
  uint64_t code_start; /* where those words start (same page) */
  uint32_t length;     /* guest instructions in the entry block: the budget needed */
  uint32_t code_words;
} Jit_Entry;

#define JIT_CACHE_BITS 18u
#define JIT_HASH_MULTIPLIER 0x9E3779B97F4A7C15ull /* Fibonacci hashing */
#define JIT_INSN_SHIFT 2u
#define JIT_HASH_BITS 64u
static inline uint64_t jit_cache_index(uint64_t pc) {
  return ((pc >> JIT_INSN_SHIFT) * JIT_HASH_MULTIPLIER) >> (JIT_HASH_BITS - JIT_CACHE_BITS);
}

/* Where compiled code finds the cache and the current code generation
 * (both are statics in linear memory, so their addresses are constants). */
typedef struct Jit_Link {
  uint64_t cache_address;      /* &Jit_Entry[0] */
  uint64_t generation_address; /* the uint64_t generation chained blocks must carry */
} Jit_Link;

typedef uint32_t (*Jit_Block_Fn)(Jit_State *state);

/* The interpreter fallback a block imports: runs `insn` at regs.pc with
 * full retirement (cycles, SVC/undefined/breakpoint handlers). Returns
 * JIT_BLOCK_CONTINUE to keep going in the block, JIT_BLOCK_STOP to end
 * the run, or JIT_HELPER_LEAVE when the block must return to the
 * dispatcher with the state complete (code may have changed). */
#define JIT_HELPER_LEAVE 2u
uint32_t jit_helper_interpret(Jit_State *state, uint32_t insn);

/* The slow half of inline loads and stores (page-crossing, or anything
 * the inline walk refuses): read `size` bytes at `address` into
 * state->scratch / store one or two `shape & 0xFF`-byte elements
 * (two if shape & 0x100) - all or nothing, exactly interp_read/
 * interp_write. 0 on a fault: the compiled code then lets the
 * interpreter take the instruction, which faults precisely. */
uint32_t jit_helper_read(Jit_State *state, uint64_t address, uint32_t size);
uint32_t jit_helper_store(Jit_State *state, uint64_t address, uint32_t shape, uint64_t first, uint64_t second);
/* Writes `size` bytes of state->scratch to `address` (all or nothing). */
uint32_t jit_helper_write(Jit_State *state, uint64_t address, uint32_t size);
/* Executes `insn` (SIMD&FP) against the state without retiring it - the
 * compiled code counts it - after the compiled code synchronised the
 * general registers and NZCV it touches. 0 if it completed; otherwise
 * nothing changed and the compiled code hands it to the interpreter. */
uint32_t jit_helper_simd(Jit_State *state, uint32_t insn);

/* ------------------------------------------------------------------ */
/* Compiler (jit_compile.c).                                           */
/* ------------------------------------------------------------------ */

typedef struct Jit_Compiled {
  uint32_t instructions; /* in the entry block: the budget needed to enter */
  uint32_t blocks;       /* region blocks */
  uint64_t code_start;   /* the instruction words the code depends on */
  uint32_t code_words;
  uint32_t module_bytes;
} Jit_Compiled;

/* Compiles the region at `pc` - the block there plus blocks in the same
 * page it reaches by direct branches - from that page's instruction words
 * `page_code` into a complete wasm module in `out`
 * (capacity `capacity`). The module imports env.memory (memory64, shared,
 * `memory_pages` pages), env.table (the core's 64-bit function table, for
 * chaining) and env.interpret/read/store/write/simd (the jit_helper_* functions), and
 * exports the region function as "b". Returns false if nothing could be
 * compiled (buffer too small). */
bool jit_compile_block(uint64_t pc, const uint32_t *page_code, uint64_t memory_pages, const Jit_Link *link,
                       uint8_t *out, uint32_t capacity, Jit_Compiled *result);


#endif /* SWITCH_CPU_BACKENDS_JIT_JIT_INTERNAL_H */
