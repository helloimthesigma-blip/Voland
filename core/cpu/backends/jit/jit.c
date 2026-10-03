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
 * Code validity is the interpreter's: blocks are compiled only from
 * executable, non-writable pages, and interp_code_generation() (vmm
 * mapping changes, IC maintenance, invalidate/clear_cache) retires them.
 * Retired functions leave the table when their cache slot is reused.
 */
#include "cpu/backends/jit/jit.h"

#include <stdlib.h>

#include "common/assert.h"
#include "cpu/backends/jit/jit_internal.h"
#include "cpu/backends/jit/jit_wasm.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/heap.h>
#endif

#define JIT_CACHE_ENTRIES 65536u
#define JIT_CACHE_INDEX(pc) (((pc) >> 2) & (JIT_CACHE_ENTRIES - 1u))
#define JIT_DEFAULT_HOT_THRESHOLD 16u
#define JIT_NEVER UINT32_MAX /* hits value: do not try to compile */
#define JIT_MODULE_BYTES (512u * 1024u)

typedef struct Jit_Entry {
  uint64_t pc;
  uint64_t generation; /* 0 = empty */
  uint64_t function;   /* table index, 0 = not compiled */
  uint32_t length;     /* guest instructions in the compiled block */
  uint32_t hits;
} Jit_Entry;

static Jit_Entry g_cache[JIT_CACHE_ENTRIES];
static uint8_t g_module[JIT_MODULE_BYTES];
static uint32_t g_hot_threshold = JIT_DEFAULT_HOT_THRESHOLD;
static Jit_Stats g_stats;

void jit_set_hot_threshold(uint32_t executions) { g_hot_threshold = executions ? executions : 1u; }
const Jit_Stats *jit_stats(void) { return &g_stats; }

/* ------------------------------------------------------------------ */
/* Installing modules.                                                 */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__
EM_JS_DEPS(voland_jit, "$addFunction,$removeFunction")

/* Compiles and instantiates the module synchronously (fine off the main
 * thread, which is where the core runs) and returns the table index of
 * its exported block function, or 0 on failure. */
EM_JS(int64_t, jit_js_install, (const uint8_t *bytes, size_t length, void *interpret), {
  try {
    const start = Number(bytes);
    const module = new WebAssembly.Module(HEAPU8.slice(start, start + Number(length)));
    const instance = new WebAssembly.Instance(module, {
      env: {memory: wasmMemory, interpret: wasmTable.get(interpret)},
    });
    return BigInt(addFunction(instance.exports.b, 'ip'));
  } catch (error) {
    console.error('[jit] module rejected:', error);
    return 0n;
  }
})

EM_JS(void, jit_js_remove, (int64_t index), { removeFunction(Number(index)); })

static uint64_t install(const uint8_t *bytes, uint32_t length) {
  return (uint64_t)jit_js_install(bytes, length, (void *)jit_helper_interpret);
}
static void uninstall(uint64_t function) { jit_js_remove((int64_t)function); }
static uint64_t memory_pages(void) { return (uint64_t)emscripten_get_heap_size() / WASM_PAGE_BYTES; }
#else
static uint64_t install(const uint8_t *bytes, uint32_t length) {
  (void)bytes;
  (void)length;
  return 0;
}
static void uninstall(uint64_t function) { (void)function; }
static uint64_t memory_pages(void) { return 0; }
#endif

/* ------------------------------------------------------------------ */
/* The interpreter fallback compiled blocks import.                    */
/* ------------------------------------------------------------------ */

uint32_t jit_helper_interpret(Jit_State *state, uint32_t insn) {
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

/* ------------------------------------------------------------------ */
/* Compilation.                                                        */
/* ------------------------------------------------------------------ */

static void compile_entry(const Interp_State *s, Jit_Entry *e) {
  e->hits = JIT_NEVER;
  const uint64_t pc = e->pc;
  const uint64_t pte = vmm_pte_inline(s->l1, pc);
  if ((pte & VMM_PERM_X) == 0 || (pte & VMM_PERM_W) != 0 || (pc & 3u)) return;
  VMM_Fault fault;
  const uint8_t *host = vmm_translate_inline(s->l1, pc, VMM_PERM_X, &fault);
  if (!host) return;
  const uint32_t available = (uint32_t)((((pc | VMM_PAGE_OFFSET_MASK) + 1u) - pc) / sizeof(uint32_t));
  Jit_Compiled compiled;
  if (!jit_compile_block(pc, (const uint32_t *)(const void *)host, available, memory_pages(), g_module,
                         JIT_MODULE_BYTES, &compiled)) {
    g_stats.compile_failures++;
    return;
  }
  const uint64_t function = install(g_module, compiled.module_bytes);
  if (!function) {
    g_stats.compile_failures++;
    return;
  }
  e->function = function;
  e->length = compiled.instructions;
  g_stats.blocks_compiled++;
  g_stats.module_bytes += compiled.module_bytes;
}

/* The entry for `pc` in this generation, (re)initialised if stale. */
static Jit_Entry *lookup(uint64_t pc, uint64_t generation) {
  Jit_Entry *e = &g_cache[JIT_CACHE_INDEX(pc)];
  if (e->pc == pc && e->generation == generation) return e;
  if (e->function) uninstall(e->function);
  e->pc = pc;
  e->generation = generation;
  e->function = 0;
  e->length = 0;
  e->hits = 0;
  return e;
}

/* ------------------------------------------------------------------ */
/* Backend vtable.                                                     */
/* ------------------------------------------------------------------ */

static CPU_ExitReason jit_run(CPU_State *state, uint64_t cycle_budget) {
  Jit_State *j = (Jit_State *)state;
  Interp_State *s = &j->interp;
  SWITCH_ASSERT_ALWAYS(s->l1 != NULL, "jit run() without a vmm");
  if (!interp_predecode_enabled()) return CPU_BACKEND_INTERPRETER.run(state, cycle_budget);
  s->cycles_consumed = 0;
  s->exclusive_valid = false; /* a potential context switch (§7) */
  uint32_t grace = 0;
  CPU_ExitReason exit_reason = CPU_EXIT_CYCLES_ELAPSED;
  for (;;) {
    Jit_Entry *e = lookup(s->regs.pc, interp_code_generation());
    if (e->function) {
      if (s->cycles_consumed + e->length <= cycle_budget) {
        g_stats.block_entries++;
        const Jit_Block_Fn fn = (Jit_Block_Fn)(uintptr_t)e->function;
        if (fn(j) == JIT_BLOCK_STOP) return j->exit_reason;
        continue;
      }
    } else if (e->hits != JIT_NEVER && ++e->hits >= g_hot_threshold) {
      compile_entry(s, e);
      if (e->function) continue;
    }
    g_stats.interpreted_blocks++;
    if (!interp_predecode_run_block(s, cycle_budget, &grace, &exit_reason)) return exit_reason;
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
};
