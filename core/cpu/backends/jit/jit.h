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

/* CPU_BACKEND_JIT is declared in cpu.h. */

#endif /* SWITCH_CPU_BACKENDS_JIT_JIT_H */
