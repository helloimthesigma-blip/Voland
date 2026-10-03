/**
 * CPU_BACKEND_JIT (jit.h, docs/JIT.md). The state is the interpreter's
 * Interp_State, so every accessor, the SVC/undefined/breakpoint plumbing
 * and the cache-maintenance entry points are the interpreter's own; only
 * run() differs.
 */
#include "cpu/backends/jit/jit.h"

#include "cpu/backends/interpreter/interp_internal.h"

static CPU_ExitReason jit_run(CPU_State *state, uint64_t cycle_budget) {
  return CPU_BACKEND_INTERPRETER.run(state, cycle_budget);
}

static CPU_State *jit_create(VMM_Context *vmm, void *userdata) { return CPU_BACKEND_INTERPRETER.create(vmm, userdata); }
static void jit_destroy(CPU_State *state) { CPU_BACKEND_INTERPRETER.destroy(state); }
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
    .version = "0.1.0",
    .supports_jit = true,
};
