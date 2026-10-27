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

/* Diagnostics: guest reads (and, with `writes`, stores) landing in host
 * bytes [host, host + bytes) invoke `hook` (pc, lr, guest address, size,
 * the bytes read or stored, whether it was a store) - e.g. a shared
 * memory page a title reads (voland-cli VOLAND_WATCH_TIME), or who writes
 * a texture (VOLAND_WATCH_GPU: with `writes`, loads are reported too). One compare per access when unused; bytes
 * 0 turns it off. */
typedef void (*Interp_Read_Hook)(uint64_t pc, uint64_t lr, uint64_t address, uint32_t size, const void *data,
                                 bool write);
void interp_set_read_watch(uint64_t host, uint64_t bytes, Interp_Read_Hook hook, bool writes);

/* Diagnostics: called when the PC named by VOLAND_PC_LOG_DUMP runs (it
 * must also be in VOLAND_PC_LOG) - voland-cli dumps every thread's state
 * there, to see who was waiting and who was runnable at that moment. */
typedef void (*Interp_Pc_Hook)(void);
void interp_set_pc_log_dump(Interp_Pc_Hook hook);
/* Re-reads the VOLAND_PC_LOG* variables on next use (set after start-up:
 * the web build's ?env=). */
void interp_pc_log_reload(void);

#endif /* SWITCH_CPU_BACKENDS_INTERPRETER_INTERPRETER_H */
