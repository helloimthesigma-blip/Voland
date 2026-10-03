/**
 * CPU_BACKEND_JIT (jit.h, docs/JIT.md): the mixed-mode run loop, the
 * compiled-block cache and the wasm module installer.
 *
 * The state is Jit_State, whose first member is the interpreter's
 * Interp_State, so every accessor, the SVC/undefined/breakpoint plumbing
 * and the cache-maintenance entry points are the interpreter's own; only
 * create() and run() differ.
 *
 * run(): at each block start, a compiled block for that PC runs if the
 * remaining budget covers its full length (so budget boundaries are the
 * interpreter's, exactly); otherwise the interpreter runs one predecoded
 * block and the PC's execution count goes up. Past the hot threshold the
 * block is compiled (jit_compile.c) and installed into the module's
 * function table (Emscripten addFunction), then called through a function
 * pointer. Natively there is no wasm engine: installation fails, nothing
 * is compiled, and the loop is the interpreter's.
 *
 * Code validity: blocks are compiled only from executable, non-writable
 * pages. When interp_code_generation() moves (any vmm mapping change, IC
 * IVAU, invalidate/clear_cache) a block is not thrown away but re-checked
 * the next time its PC comes up: still executable and not writable, and
 * the same instruction bytes (a 64-bit hash of them) - compiled code
 * depends on nothing else. Games map and unmap data constantly, so
 * retiring every block per mapping change recompiled everything.
 * Retired functions leave the table when their cache slot is reused.
 */
#include "cpu/backends/jit/jit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/assert.h"
#include "common/log.h"
#include "cpu/backends/jit/jit_internal.h"
#include "cpu/backends/jit/jit_wasm.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/heap.h>
#endif

#define JIT_CACHE_ENTRIES (1u << JIT_CACHE_BITS)
#define JIT_HIT_COUNTERS (1u << 16)
/* Interpreted executions before a block is compiled. Measured on
 * Silksong (docs/handoff/JIT_STATUS.md): 16 compiled ~2.5x the modules of
 * 256 for the same guest speed, and module compilation is the JIT's
 * largest overhead while games load. */
#define JIT_DEFAULT_HOT_THRESHOLD 256u
#define JIT_MODULE_BYTES (512u * 1024u)
#define JIT_RECENT_BLOCKS 64u

#define CODE_HASH_OFFSET 0xCBF29CE484222325ull /* FNV-1a 64 */
#define CODE_HASH_PRIME 0x100000001B3ull

static uint64_t code_hash(const uint32_t *code, uint32_t count) {
  uint64_t hash = CODE_HASH_OFFSET;
  for (uint32_t i = 0; i < count; i++) hash = (hash ^ code[i]) * CODE_HASH_PRIME;
  return hash;
}

/* The block's code as it is mapped now, or NULL if it is no longer
 * executable-and-not-writable. */
static const uint32_t *block_code(const Interp_State *s, uint64_t pc) {
  const uint64_t pte = vmm_pte_inline(s->l1, pc);
  if ((pte & VMM_PERM_X) == 0 || (pte & VMM_PERM_W) != 0 || (pc & 3u)) return NULL;
  VMM_Fault fault;
  return (const uint32_t *)(const void *)vmm_translate_inline(s->l1, pc, VMM_PERM_X, &fault);
}

/* Everything compiled code depends on, per host thread. Compiled
 * functions live in the function table of the host thread that installed
 * them, so with parallel guest threads (docs/PARALLEL.md) every core
 * compiles into its own cache - and each module bakes in its own thread's
 * cache and generation addresses (Jit_Link), so chaining never leaves the
 * thread either. The first thread to run uses the static instance; any
 * other allocates one once, released (functions removed from its table)
 * when the thread exits. */
typedef struct Jit_Thread {
  /* Compiled blocks only; cold code is counted in hits (hashed, untagged:
   * an alias just makes a block hot a little early). */
  Jit_Entry cache[JIT_CACHE_ENTRIES];
  uint16_t hits[JIT_HIT_COUNTERS];
  /* The code generation the dispatcher last saw; chained blocks only enter
   * entries validated in it (jit_compile.c, emit_chain). */
  uint64_t generation;
  uint8_t module[JIT_MODULE_BYTES];
  /* The dispatcher's last block starts (bit 63: a compiled region was
   * entered), for the crash report. */
  uint64_t recent[JIT_RECENT_BLOCKS];
  uint32_t recent_next;
} Jit_Thread;

#define JIT_RECENT_COMPILED (1ull << 63)

static void note_block(Jit_Thread *t, uint64_t pc_and_flag) {
  t->recent[t->recent_next] = pc_and_flag;
  t->recent_next = (t->recent_next + 1u) % JIT_RECENT_BLOCKS;
}

/* A run ended in a fault or breakpoint: the registers and how the
 * dispatcher got there (chained regions do not pass through it, so a
 * compiled entry may have run through several regions). */
static void report_stop(const Jit_Thread *t, const Interp_State *s, CPU_ExitReason reason) {
  log_error("[jit] %s at pc=0x%010llx (fault address 0x%010llx), sp=0x%010llx nzcv=%x",
            reason == CPU_EXIT_FAULT ? "fault" : "breakpoint", (unsigned long long)s->regs.pc,
            (unsigned long long)s->fault_address, (unsigned long long)s->regs.sp, s->regs.pstate >> 28);
  for (uint32_t r = 0; r < 31u; r += 4u) {
    log_error("[jit]   x%-2u %016llx %016llx %016llx %016llx", r, (unsigned long long)s->regs.x[r],
              (unsigned long long)(r + 1u < 31u ? s->regs.x[r + 1u] : 0), (unsigned long long)(r + 2u < 31u ? s->regs.x[r + 2u] : 0),
              (unsigned long long)(r + 3u < 31u ? s->regs.x[r + 3u] : 0));
  }
  char line[JIT_RECENT_BLOCKS * 24u];
  uint32_t used = 0;
  for (uint32_t i = 0; i < JIT_RECENT_BLOCKS; i++) {
    const uint64_t v = t->recent[(t->recent_next + i) % JIT_RECENT_BLOCKS];
    if (!v) continue;
    used += (uint32_t)snprintf(line + used, sizeof(line) - used, " %s%llx", (v & JIT_RECENT_COMPILED) ? "*" : "",
                               (unsigned long long)(v & ~JIT_RECENT_COMPILED));
    if (used >= sizeof(line)) break;
  }
  log_error("[jit] recent block starts (oldest first, * = compiled region entered):%s", line);
}

static Jit_Thread g_main_thread;
static uint32_t g_hot_threshold = JIT_DEFAULT_HOT_THRESHOLD;
static Jit_Stats g_stats;

void jit_set_hot_threshold(uint32_t executions) { g_hot_threshold = executions ? executions : 1u; }

static bool g_hot_profile;
void jit_set_hot_profile(bool enabled) { g_hot_profile = enabled; }

void jit_print_hot_regions(uint32_t top) {
  const Jit_Entry *g_cache = g_main_thread.cache; /* the first core's (the CLI runs one) */
  static bool printed[JIT_CACHE_ENTRIES];
  memset(printed, 0, sizeof(printed));
  for (uint32_t n = 0; n < top; n++) {
    uint32_t best = JIT_CACHE_ENTRIES;
    uint64_t best_weight = 0;
    for (uint32_t i = 0; i < JIT_CACHE_ENTRIES; i++) {
      const uint64_t weight = g_cache[i].entries * g_cache[i].length;
      if (g_cache[i].function && !printed[i] && weight > best_weight) {
        best = i;
        best_weight = weight;
      }
    }
    if (best == JIT_CACHE_ENTRIES) return;
    printed[best] = true;
    fprintf(stderr, "  %010llx  %12llu entries  entry block %3u insns  region %4u words\n",
            (unsigned long long)g_cache[best].pc, (unsigned long long)g_cache[best].entries, g_cache[best].length,
            g_cache[best].code_words);
  }
}

static const char *g_dump_directory;
void jit_set_dump_directory(const char *directory) { g_dump_directory = directory; }

#define DUMP_PATH_BYTES 512u
static void dump_module(uint64_t pc, const uint8_t *bytes, uint32_t length) {
  char path[DUMP_PATH_BYTES];
  snprintf(path, sizeof(path), "%s/%010llx.wasm", g_dump_directory, (unsigned long long)pc);
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fwrite(bytes, 1, length, f);
  fclose(f);
}
const Jit_Stats *jit_stats(void) { return &g_stats; }

/* The guest code a region was compiled from: DIR/<pc>.s, for an
 * assembler + disassembler (".word" per instruction, the address in a
 * comment). */
static void dump_code(uint64_t pc, uint64_t start, const uint32_t *words, uint32_t count) {
  char path[DUMP_PATH_BYTES];
  snprintf(path, sizeof(path), "%s/%010llx.s", g_dump_directory, (unsigned long long)pc);
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "// region %llx, code from %llx\n", (unsigned long long)pc, (unsigned long long)start);
  for (uint32_t i = 0; i < count; i++) {
    fprintf(f, ".word 0x%08x // %llx\n", words[i], (unsigned long long)(start + 4u * i));
  }
  fclose(f);
}

/* ------------------------------------------------------------------ */
/* Installing modules.                                                 */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
EM_JS_DEPS(voland_jit, "$addFunction,$removeFunction")

/* Compiles and instantiates the module synchronously (fine off the main
 * thread, which is where the core runs) and returns the table index of
 * its exported block function, or 0 on failure. */
EM_JS(int64_t, jit_js_install,
      (const uint8_t *bytes, size_t length, void *interpret, void *read, void *store, void *write, void *simd), {
  try {
    /* WebAssembly.Module refuses views of shared memory: copy into one
     * reused, growing, non-shared buffer rather than a new one per module. */
    const start = Number(bytes), size = Number(length);
    if (!globalThis.volandJitBytes || globalThis.volandJitBytes.length < size) {
      globalThis.volandJitBytes = new Uint8Array(Math.max(size, 65536) * 2);
    }
    const staging = globalThis.volandJitBytes.subarray(0, size);
    staging.set(HEAPU8.subarray(start, start + size));
    const module = new WebAssembly.Module(staging);
    const instance = new WebAssembly.Instance(module, {
      env: {
        memory: wasmMemory,
        table: wasmTable,
        interpret: wasmTable.get(interpret),
        read: wasmTable.get(read),
        store: wasmTable.get(store),
        write: wasmTable.get(write),
        simd: wasmTable.get(simd),
      },
    });
    return BigInt(addFunction(instance.exports.b, 'ip'));
  } catch (error) {
    console.error('[jit] module rejected:', error);
    return 0n;
  }
})

EM_JS(void, jit_js_remove, (int64_t index), { removeFunction(Number(index)); })

static uint64_t install(const uint8_t *bytes, uint32_t length) {
  return (uint64_t)jit_js_install(bytes, length, (void *)jit_helper_interpret, (void *)jit_helper_read,
                                  (void *)jit_helper_store, (void *)jit_helper_write, (void *)jit_helper_simd);
}
static void uninstall(uint64_t function) { jit_js_remove((int64_t)function); }
static uint64_t memory_pages(void) { return (uint64_t)emscripten_get_heap_size() / WASM_PAGE_BYTES; }
static bool can_install(void) { return true; }
#else
static uint64_t install(const uint8_t *bytes, uint32_t length) {
  (void)bytes;
  (void)length;
  return 0;
}
static void uninstall(uint64_t function) { (void)function; }
static uint64_t memory_pages(void) { return 0; }
static bool can_install(void) { return false; } /* no wasm engine natively */
#endif

/* ------------------------------------------------------------------ */
/* Per-host-thread state.                                              */
/* ------------------------------------------------------------------ */

#if (defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)) || defined(_WIN32)
static Jit_Thread *thread_jit(void) { return &g_main_thread; }
static void compiler_lock(void) {}
static void compiler_unlock(void) {}
#else
#include <pthread.h>

/* jit_compile_block keeps its working state (contexts, the analysis
 * buffer) in statics: one compilation at a time across host threads. */
static pthread_mutex_t g_compiler = PTHREAD_MUTEX_INITIALIZER;
static void compiler_lock(void) { pthread_mutex_lock(&g_compiler); }
static void compiler_unlock(void) { pthread_mutex_unlock(&g_compiler); }

static _Thread_local Jit_Thread *t_jit;
static bool g_main_claimed;
static pthread_key_t g_jit_key;
static pthread_once_t g_jit_key_once = PTHREAD_ONCE_INIT;

/* A core thread exits: its functions leave its table (the worker may be
 * reused), then its cache goes. */
static void release_thread_jit(void *data) {
  Jit_Thread *t = (Jit_Thread *)data;
  for (uint32_t i = 0; i < JIT_CACHE_ENTRIES; i++) {
    if (t->cache[i].function) uninstall(t->cache[i].function);
  }
  free(t);
}

static void make_jit_key(void) { (void)pthread_key_create(&g_jit_key, release_thread_jit); }

/* NULL only if an extra thread cannot get memory for its cache. */
static Jit_Thread *thread_jit(void) {
  if (t_jit) return t_jit;
  if (!__atomic_exchange_n(&g_main_claimed, true, __ATOMIC_ACQ_REL)) {
    t_jit = &g_main_thread;
    return t_jit;
  }
  Jit_Thread *t = (Jit_Thread *)calloc(1, sizeof(Jit_Thread)); /* once per extra host thread */
  if (!t) return NULL;
  (void)pthread_once(&g_jit_key_once, make_jit_key);
  (void)pthread_setspecific(g_jit_key, t);
  t_jit = t;
  return t_jit;
}
#endif

/* ------------------------------------------------------------------ */
/* The interpreter fallback compiled blocks import.                    */
/* ------------------------------------------------------------------ */

/* Fallback profile: open addressing over masked encodings. */
#define FALLBACK_SLOTS 4096u
#define FALLBACK_OPCODE_MASK 0xFFE0FC00u /* drops Rd, Rn, Rm */
static bool g_fallback_profile;
static uint32_t g_fallback_key[FALLBACK_SLOTS];
static uint64_t g_fallback_count[FALLBACK_SLOTS];

void jit_set_fallback_profile(bool enabled) { g_fallback_profile = enabled; }

static void profile_fallback(uint32_t insn) {
  const uint32_t key = (insn & FALLBACK_OPCODE_MASK) | 1u; /* never 0: 0 = empty slot */
  for (uint32_t i = 0, slot = (key * 2654435761u) >> 20; i < FALLBACK_SLOTS; i++, slot = (slot + 1u) % FALLBACK_SLOTS) {
    if (g_fallback_key[slot] == key || g_fallback_key[slot] == 0) {
      g_fallback_key[slot] = key;
      g_fallback_count[slot]++;
      return;
    }
  }
}

void jit_print_fallback_profile(uint32_t top) {
  for (uint32_t n = 0; n < top; n++) {
    uint32_t best = FALLBACK_SLOTS;
    for (uint32_t i = 0; i < FALLBACK_SLOTS; i++) {
      if (g_fallback_count[i] && (best == FALLBACK_SLOTS || g_fallback_count[i] > g_fallback_count[best])) best = i;
    }
    if (best == FALLBACK_SLOTS) return;
    fprintf(stderr, "  %12llu  %08x\n", (unsigned long long)g_fallback_count[best], g_fallback_key[best] & ~1u);
    g_fallback_count[best] = 0;
  }
}

static void count_helper(uint32_t insn) {
  if (g_fallback_profile) profile_fallback(insn);
  switch (bits(insn, 28, 25)) {
  case 0x7: case 0xF: g_stats.helper_simd_fp++; break;
  case 0x4: case 0x6: case 0xC: case 0xE:
    if (bit(insn, 26)) g_stats.helper_memory_simd++;
    else if (bits(insn, 29, 28) == 0) g_stats.helper_memory_exclusive++;
    else g_stats.helper_memory++;
    break;
  case 0xA: case 0xB: g_stats.helper_system++; break;
  default: g_stats.helper_other++; break;
  }
}

uint32_t jit_helper_interpret(Jit_State *state, uint32_t insn) {
  count_helper(insn);
  Interp_State *s = &state->interp;
  const uint64_t pc = s->regs.pc;
  CPU_ExitReason exit_reason = CPU_EXIT_CYCLES_ELAPSED;
  if (!interp_retire(s, interp_execute(s, insn), pc, insn, &exit_reason)) {
    state->exit_reason = exit_reason;
    return JIT_BLOCK_STOP;
  }
  if (interp_is_cache_maintenance(insn)) {
    interp_predecode_flush();
    return JIT_HELPER_LEAVE;
  }
  return JIT_BLOCK_CONTINUE;
}

uint32_t jit_helper_read(Jit_State *state, uint64_t address, uint32_t size) {
  return interp_read(&state->interp, address, state->scratch, size) ? 1u : 0u;
}

uint32_t jit_helper_simd(Jit_State *state, uint32_t insn) {
  if (g_fallback_profile) profile_fallback(insn);
  if (state->interp.fpcr != 0) g_stats.simd_fpcr_nonzero++;
  else if (!(state->interp.fpsr & 0x10u)) g_stats.simd_ixc_clear++;
  g_stats.last_fpcr = state->interp.fpcr;
  g_stats.direct_simd++;
  return interp_execute(&state->interp, insn) == INTERP_CONTINUE ? 0u : 1u;
}

uint32_t jit_helper_write(Jit_State *state, uint64_t address, uint32_t size) {
  return interp_write(&state->interp, address, state->scratch, size) ? 1u : 0u;
}

#define STORE_SHAPE_SIZE_MASK 0xFFu
#define STORE_SHAPE_PAIR 0x100u
#define BITS_PER_BYTE 8u

uint32_t jit_helper_store(Jit_State *state, uint64_t address, uint32_t shape, uint64_t first, uint64_t second) {
  const uint32_t size = shape & STORE_SHAPE_SIZE_MASK;
  uint8_t data[2u * sizeof(uint64_t)];
  for (uint32_t i = 0; i < size; i++) {
    data[i] = (uint8_t)(first >> (BITS_PER_BYTE * i));
    data[size + i] = (uint8_t)(second >> (BITS_PER_BYTE * i));
  }
  const uint32_t total = (shape & STORE_SHAPE_PAIR) ? 2u * size : size;
  return interp_write(&state->interp, address, data, total) ? 1u : 0u;
}

/* ------------------------------------------------------------------ */
/* Compilation.                                                        */
/* ------------------------------------------------------------------ */

/* Compiles the block at `pc` into its cache slot (evicting whatever is
 * there). */
static void compile(Jit_Thread *t, const Interp_State *s, uint64_t pc, uint64_t generation) {
  const uint32_t *code = block_code(s, pc);
  if (!code) return;
  const uint32_t *page_code = code - (pc & VMM_PAGE_OFFSET_MASK) / sizeof(uint32_t);
  Jit_Link link;
  link.cache_address = (uint64_t)(uintptr_t)t->cache;
  link.generation_address = (uint64_t)(uintptr_t)&t->generation;
  link.count_entries = g_hot_profile;
  Jit_Compiled compiled;
  compiler_lock();
  const bool built = jit_compile_block(pc, page_code, memory_pages(), &link, t->module, JIT_MODULE_BYTES, &compiled);
  compiler_unlock();
  if (!built) {
    g_stats.compile_failures++;
    return;
  }
  if (g_dump_directory) {
    dump_module(pc, t->module, compiled.module_bytes);
    dump_code(pc, compiled.code_start, page_code + (compiled.code_start & VMM_PAGE_OFFSET_MASK) / sizeof(uint32_t),
              compiled.code_words);
  }
  const uint64_t function = install(t->module, compiled.module_bytes);
  if (!function) {
    g_stats.compile_failures++;
    return;
  }
  Jit_Entry *e = &t->cache[jit_cache_index(pc)];
  if (e->function) {
    uninstall(e->function);
    g_stats.evictions++;
  }
  e->pc = pc;
  e->generation = generation;
  e->function = function;
  e->length = compiled.instructions;
  e->code_start = compiled.code_start;
  e->code_words = compiled.code_words;
  e->entries = 0;
  e->multicore = cpu_multicore();
  e->code_hash = code_hash(page_code + (compiled.code_start & VMM_PAGE_OFFSET_MASK) / sizeof(uint32_t),
                           compiled.code_words);
  g_stats.blocks_compiled++;
  g_stats.region_blocks += compiled.blocks;
  g_stats.module_bytes += compiled.module_bytes;
}

/* The compiled block for `pc`, valid in `generation`, or NULL. A block
 * from an older generation is revalidated here. */
static Jit_Entry *find(Jit_Thread *t, const Interp_State *s, uint64_t pc, uint64_t generation) {
  Jit_Entry *e = &t->cache[jit_cache_index(pc)];
  if (e->pc != pc || !e->function) return NULL;
  if (e->generation == generation) return e;
  if (e->multicore != cpu_multicore()) goto stale; /* exclusives/fences were compiled for the other mode */
  const uint32_t *code = block_code(s, e->code_start);
  if (code && code_hash(code, e->code_words) == e->code_hash) {
    e->generation = generation;
    g_stats.revalidations++;
    return e;
  }
stale:
  g_stats.stale++;
  uninstall(e->function);
  e->function = 0;
  e->pc = 0;
  return NULL;
}

/* ------------------------------------------------------------------ */
/* Backend vtable.                                                     */
/* ------------------------------------------------------------------ */

static CPU_ExitReason jit_run(CPU_State *state, uint64_t cycle_budget) {
  Jit_State *j = (Jit_State *)state;
  Interp_State *s = &j->interp;
  SWITCH_ASSERT_ALWAYS(s->l1 != NULL, "jit run() without a vmm");
  /* Multicore (docs/PARALLEL.md): each host thread compiles into and runs
   * from its own cache (Jit_Thread), and code compiled while
   * cpu_multicore() is set makes store-exclusives a compare-and-swap and
   * barriers fences (jit_compile.c, c_exclusive / c_system). */
  Jit_Thread *const t = thread_jit();
  if (!t || !interp_predecode_enabled() || !can_install()) {
    return CPU_BACKEND_INTERPRETER.run(state, cycle_budget);
  }
  s->cycles_consumed = 0;
  s->exclusive_valid = false; /* a potential context switch (§7) */
  j->cycle_budget = cycle_budget;
  uint32_t grace = 0;
  CPU_ExitReason exit_reason = CPU_EXIT_CYCLES_ELAPSED;
  for (;;) {
    const uint64_t generation = interp_code_generation();
    if (generation != t->generation) {
      t->generation = generation;
      g_stats.generations++;
    }
    const uint64_t pc = s->regs.pc;
    const Jit_Entry *e = find(t, s, pc, generation);
    if (e) {
      if (s->cycles_consumed + e->length <= cycle_budget) {
        g_stats.block_entries++;
        note_block(t, pc | JIT_RECENT_COMPILED);
        const Jit_Block_Fn fn = (Jit_Block_Fn)(uintptr_t)e->function;
        if (fn(j) == JIT_BLOCK_STOP) {
          if (j->exit_reason == CPU_EXIT_FAULT || j->exit_reason == CPU_EXIT_BREAKPOINT) report_stop(t, s, j->exit_reason);
          return j->exit_reason;
        }
        continue;
      }
    } else {
      uint16_t *hits = &t->hits[jit_cache_index(pc) & (JIT_HIT_COUNTERS - 1u)];
      if (++*hits >= g_hot_threshold) {
        *hits = 0;
        compile(t, s, pc, generation);
        if (find(t, s, pc, generation)) continue;
      }
    }
    g_stats.interpreted_blocks++;
    note_block(t, pc);
    if (!interp_predecode_run_block(s, cycle_budget, &grace, &exit_reason)) {
      if (exit_reason == CPU_EXIT_FAULT || exit_reason == CPU_EXIT_BREAKPOINT) report_stop(t, s, exit_reason);
      return exit_reason;
    }
  }
}

static CPU_State *jit_create(VMM_Context *vmm, void *userdata) {
  Jit_State *j = (Jit_State *)calloc(1, sizeof(Jit_State));
  if (!j) return NULL;
  j->interp.vmm = vmm;
  j->interp.l1 = vmm ? vmm_page_table_l1(vmm) : NULL;
  j->interp.userdata = userdata;
  return (CPU_State *)j;
}

static void jit_destroy(CPU_State *state) { free(state); }
static CPU_ExitReason jit_step(CPU_State *state) { return CPU_BACKEND_INTERPRETER.step(state); }
static uint64_t jit_get_fault_address(CPU_State *state) { return CPU_BACKEND_INTERPRETER.get_fault_address(state); }
static uint64_t jit_get_cycles_consumed(CPU_State *state) { return CPU_BACKEND_INTERPRETER.get_cycles_consumed(state); }
static uint64_t jit_get_reg(CPU_State *state, uint8_t index) { return CPU_BACKEND_INTERPRETER.get_reg(state, index); }
static void jit_set_reg(CPU_State *state, uint8_t index, uint64_t value) {
  CPU_BACKEND_INTERPRETER.set_reg(state, index, value);
}
static uint64_t jit_get_pc(CPU_State *state) { return CPU_BACKEND_INTERPRETER.get_pc(state); }
static void jit_set_pc(CPU_State *state, uint64_t value) { CPU_BACKEND_INTERPRETER.set_pc(state, value); }
static uint64_t jit_get_sp(CPU_State *state) { return CPU_BACKEND_INTERPRETER.get_sp(state); }
static void jit_set_sp(CPU_State *state, uint64_t value) { CPU_BACKEND_INTERPRETER.set_sp(state, value); }
static uint32_t jit_get_pstate(CPU_State *state) { return CPU_BACKEND_INTERPRETER.get_pstate(state); }
static void jit_set_pstate(CPU_State *state, uint32_t value) { CPU_BACKEND_INTERPRETER.set_pstate(state, value); }
static CPU_Register_File *jit_get_register_file(CPU_State *state) {
  return CPU_BACKEND_INTERPRETER.get_register_file(state);
}
static uint64_t jit_get_sys_reg(CPU_State *state, uint32_t reg) { return CPU_BACKEND_INTERPRETER.get_sys_reg(state, reg); }
static void jit_set_sys_reg(CPU_State *state, uint32_t reg, uint64_t value) {
  CPU_BACKEND_INTERPRETER.set_sys_reg(state, reg, value);
}
static CPU_Vector_Register jit_get_vector_reg(CPU_State *state, uint8_t index) {
  return CPU_BACKEND_INTERPRETER.get_vector_reg(state, index);
}
static void jit_set_vector_reg(CPU_State *state, uint8_t index, CPU_Vector_Register value) {
  CPU_BACKEND_INTERPRETER.set_vector_reg(state, index, value);
}
static void jit_invalidate_cache(CPU_State *state, uint64_t address, uint64_t size) {
  CPU_BACKEND_INTERPRETER.invalidate_cache(state, address, size);
}
static void jit_clear_cache(CPU_State *state) { CPU_BACKEND_INTERPRETER.clear_cache(state); }
static void jit_set_svc_handler(CPU_State *state, CPU_SVC_Handler h) { CPU_BACKEND_INTERPRETER.set_svc_handler(state, h); }
static void jit_set_undefined_handler(CPU_State *state, CPU_Undefined_Handler h) {
  CPU_BACKEND_INTERPRETER.set_undefined_handler(state, h);
}
static void jit_set_breakpoint_handler(CPU_State *state, CPU_Breakpoint_Handler h) {
  CPU_BACKEND_INTERPRETER.set_breakpoint_handler(state, h);
}

const CPU_Backend CPU_BACKEND_JIT = {
    .create = jit_create,
    .destroy = jit_destroy,
    .run = jit_run,
    .step = jit_step,
    .get_fault_address = jit_get_fault_address,
    .get_cycles_consumed = jit_get_cycles_consumed,
    .get_reg = jit_get_reg,
    .set_reg = jit_set_reg,
    .get_pc = jit_get_pc,
    .set_pc = jit_set_pc,
    .get_sp = jit_get_sp,
    .set_sp = jit_set_sp,
    .get_pstate = jit_get_pstate,
    .set_pstate = jit_set_pstate,
    .get_register_file = jit_get_register_file,
    .get_sys_reg = jit_get_sys_reg,
    .set_sys_reg = jit_set_sys_reg,
    .get_vector_reg = jit_get_vector_reg,
    .set_vector_reg = jit_set_vector_reg,
    .invalidate_cache = jit_invalidate_cache,
    .clear_cache = jit_clear_cache,
    .set_svc_handler = jit_set_svc_handler,
    .set_undefined_handler = jit_set_undefined_handler,
    .set_breakpoint_handler = jit_set_breakpoint_handler,
    .name = "jit",
    .version = "0.2.0",
    .supports_jit = true,
    .supports_multicore = true, /* per-host-thread code caches (Jit_Thread) */
};
