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
  /* The running host thread's compiled-block cache (&Jit_Entry[0]), set at
   * every run() entry: compiled code reads it here rather than baking an
   * address in, so a region compiles to the same bytes on every core and
   * the engine shares one compiled module between them (docs/PARALLEL.md). */
  uint64_t thread_cache;
  uint32_t isa; /* JIT_ISA_*: what the state's code is (an A32 state is cpu/backends/a32's A32_State) */
} Jit_State;

#define JIT_ISA_A64 0u
#define JIT_ISA_A32 1u

/* A region may span this many code pages (calls and PLT stubs). */
#define JIT_MAX_REGION_PAGES 4u

/* Instruction words compiled code depends on (all in one page). */
typedef struct Jit_Code_Range {
  uint64_t start;
  uint32_t words;
} Jit_Code_Range;

/* The compiled-block cache (jit.c), direct-mapped by a hash of the PC.
 * Compiled blocks read it too: a block's exit looks its successor up and
 * tail-calls it when it is compiled, current and fits the budget
 * (jit_compile.c), so the dispatcher in C only sees the misses. */
typedef struct Jit_Entry {
  uint64_t pc;
  uint64_t generation; /* the code generation it was last validated in */
  uint64_t function;   /* table index; 0 = empty */
  uint64_t code_hash;  /* of the instruction words the code depends on (jit.c's ranges) */
  uint32_t length;     /* guest instructions in the entry block: the budget needed */
  uint32_t range_count;
  Jit_Code_Range ranges[JIT_MAX_REGION_PAGES]; /* the code it was compiled from */
  uint64_t entries;    /* times entered (only counted with jit_set_hot_profile) */
  bool multicore;      /* compiled under cpu_multicore() (exclusives, fences) */
  uint8_t referenced;  /* entered since the replacement hand last passed (second chance) */
  uint32_t pending;    /* an asynchronous compilation for this pc is in flight (its id) */
} Jit_Entry;

#define JIT_CACHE_BITS 17u
#define JIT_HASH_MULTIPLIER 0x9E3779B97F4A7C15ull /* Fibonacci hashing */
#define JIT_INSN_SHIFT 2u
#define JIT_HASH_BITS 64u
static inline uint64_t jit_cache_index(uint64_t pc) {
  return ((pc >> JIT_INSN_SHIFT) * JIT_HASH_MULTIPLIER) >> (JIT_HASH_BITS - JIT_CACHE_BITS);
}

/* The cache is JIT_CACHE_WAYS-way set associative: a pc may live in any
 * way of its set (entries set*WAYS .. set*WAYS + WAYS-1). A direct-mapped
 * cache let hot regions that share a slot evict each other forever
 * (measured: as many evictions as compiles in steady gameplay). */
#define JIT_CACHE_WAY_BITS 2u
#define JIT_CACHE_WAYS (1u << JIT_CACHE_WAY_BITS)
#define JIT_CACHE_SET_BITS (JIT_CACHE_BITS - JIT_CACHE_WAY_BITS)
static inline uint64_t jit_cache_set(uint64_t pc) {
  return ((pc >> JIT_INSN_SHIFT) * JIT_HASH_MULTIPLIER) >> (JIT_HASH_BITS - JIT_CACHE_SET_BITS);
}

/* Where compiled code finds the cache and the current code generation
 * (both are statics in linear memory, so their addresses are constants). */
typedef struct Jit_Link {
  uint64_t cache_address;      /* &Jit_Entry[0] */
  uint64_t generation_address; /* the uint64_t generation chained blocks must carry */
  bool count_entries;          /* chained entries increment the `entries` of the entry they enter */
  bool span_calls;             /* regions follow BL/RET and predicted PLT branches */
  bool aarch32;                /* the code is A32 (the A32 front end, jit_compile_a32.inc) */
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
/* Fallback profile (diagnostics): the compiler notes each instruction it
 * gave an inline fast path, so the profile can tell guard misses (the
 * exact arm) from instructions with no fast path. */
void jit_note_fast_path(uint32_t insn);

/* ------------------------------------------------------------------ */
/* Compiler (jit_compile.c).                                           */
/* ------------------------------------------------------------------ */

typedef struct Jit_Compiled {
  uint32_t instructions; /* in the entry block: the budget needed to enter */
  uint32_t blocks;       /* region blocks */
  Jit_Code_Range ranges[JIT_MAX_REGION_PAGES];
  uint32_t range_count;
  uint32_t module_bytes;
} Jit_Compiled;

/* Where the compiler reads guest code, and guest memory for predictions
 * (the GOT slot behind a PLT stub's indirect branch; a prediction is
 * only a guarded guess, so stale memory costs speed, never correctness). */
typedef struct Jit_Code_Source {
  /* The words of the page at `page` if it is executable and not
   * writable (code that may be compiled), else NULL. */
  const uint32_t *(*page_code)(void *context, uint64_t page);
  /* 8 bytes at `address` if readable. */
  bool (*peek64)(void *context, uint64_t address, uint64_t *value);
  void *context;
} Jit_Code_Source;

/* Compiles the region at `pc` - the block there plus blocks it reaches by
 * direct branches, calls (with their returns) and predicted PLT branches,
 * in up to JIT_MAX_REGION_PAGES pages read through `source` - into a
 * complete wasm module in `out`
 * (capacity `capacity`). The module imports env.memory (memory64, shared,
 * `memory_pages` pages), env.table (the core's 64-bit function table, for
 * chaining) and env.interpret/read/store/write/simd (the jit_helper_* functions), and
 * exports the region function as "b". Returns false if nothing could be
 * compiled (buffer too small). */
bool jit_compile_block(uint64_t pc, const Jit_Code_Source *source, uint64_t memory_pages, const Jit_Link *link,
                       uint8_t *out, uint32_t capacity, Jit_Compiled *result);


#endif /* SWITCH_CPU_BACKENDS_JIT_JIT_INTERNAL_H */
