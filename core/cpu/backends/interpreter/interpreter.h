/**
 * The ARM64 interpreter backend (§8, §25 Phase 2): slow but correct, the
 * web execution path until the predecoded interpreter (Phase 5). The
 * vtable is CPU_BACKEND_INTERPRETER (cpu.h); selected with
 * -DCPU_BACKEND=interpreter.
 */
#ifndef SWITCH_CPU_BACKENDS_INTERPRETER_INTERPRETER_H
#define SWITCH_CPU_BACKENDS_INTERPRETER_INTERPRETER_H

#include <stdbool.h>

#include "cpu/cpu.h"

/* Diagnostics: calls (BL / BLR) to any of up to INTERP_TRACE_MAX
 * addresses invoke `hook` with the calling state before the jump - the
 * CLI's VOLAND_TRACE_CALLS. One compare per call when unused. */
#define INTERP_TRACE_MAX 8u
/* `return_address` is the caller's; when `returning` the hook fires on
 * the traced call's RET instead (x0 then holds the result). */
typedef void (*Interp_Call_Hook)(CPU_State *state, uint64_t target, uint64_t return_address, bool returning);
void interp_set_call_trace(const uint64_t *targets, uint32_t count, Interp_Call_Hook hook);

#endif /* SWITCH_CPU_BACKENDS_INTERPRETER_INTERPRETER_H */
