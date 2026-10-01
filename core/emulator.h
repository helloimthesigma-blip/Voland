/**
 * Top-level emulator wiring. The core entry point for every platform.
 * Phase 0 scope: create/destroy, wire the no-op CPU backend into the stub
 * HLE dispatcher, expose the bounded run/step contract.
 */
#ifndef SWITCH_EMULATOR_H
#define SWITCH_EMULATOR_H

#include <stdbool.h>
#include <stdint.h>

#include "common/layout.h"
#include "common/result.h"
#include "common/vmm.h"
#include "cpu/cpu.h"
#include "hle/hle.h"
#include "hle/kernel/page_allocator.h"
#include "hle/kernel/process.h"
#include "hle/kernel/event.h"
#include "hle/kernel/scheduler.h"
#include "hle/kernel/shared_memory.h"
#include "hle/services/hid/hid.h"
#include "hle/services/nvdrv/nvdrv.h"
#include "hle/loader/byte_source.h"

/* Scratch for one bootstrap: the ExeFS directory plus the largest
 * compressed NSO segment staged for LZ4. A shipping title's biggest
 * `main` .text compresses to ~50-60MB; created for the duration of
 * emulator_load_program() only, then freed. */
#define EMULATOR_LOADER_ARENA_BYTES ((size_t)96 * 1024 * 1024)

typedef struct Emulator
{
  /* Softmmu (§5). Created after the layout and before the CPU backend,
   * which receives it at CPU_State creation; shared by every CPU_State
   * and by HLE. */
  VMM_Context *vmm;
  const CPU_Backend *cpu_backend;
  /* One CPU_State stands in for "the" guest thread until the Phase 2
   * scheduler (§7) exists to multiplex real ones. */
  CPU_State *cpu_state;
  HLE_Context hle;

  /* Guest physical pages (§4) and the loaded process (§12). `process`
   * is valid only while `program_loaded` is true. */
  Page_Allocator pages;
  Process process;
  bool program_loaded;

  /* IPC kernel state and the sm: registry (§12), reachable from HLE
   * through hle.sessions / hle.sm. Reset by
   * emulator_unload_program (all sessions die with the process); the
   * registry is filled once at emulator_create and survives reloads. */
  IPC_Session_Pool sessions;
  SM_Registry sm;

  /* Guest threads (§7). The main thread wraps `cpu_state`; CreateThread
   * threads own their own states. Reset by emulator_unload_program. */
  Scheduler scheduler;
  Event_Pool events; /* kernel events (event.h); reset with the process */
  Nvdrv_State nvdrv; /* the nvdrv service (§13); reset with the process */
  Shared_Memory_Pool shared_memory; /* shared_memory.h; reset with the process */
  Hid_State hid;     /* the hid service (§18); reset with the process */
} Emulator;

/* What one emulator_run_slice() did (§7 scheduler status). */
typedef enum Emulator_Status {
  EMULATOR_RUNNING,  /* a guest thread ran */
  EMULATOR_IDLE,     /* every thread sleeps; virtual time jumped ahead */
  EMULATOR_EXITED,   /* ExitProcess or every thread exited */
  EMULATOR_CRASHED,  /* svcBreak or an unhandled fault/undefined instruction */
  EMULATOR_DEADLOCK, /* every thread waits forever */
  EMULATOR_NOT_LOADED,
} Emulator_Status;

/* Reserves the linear memory layout (§4), creates the softmmu (§5), and
 * wires the active CPU backend (§8) to the stub HLE dispatcher. There is
 * no Emulator_Config: guest RAM size and every other region size are
 * fixed by common/layout.h, not caller-configurable - on web the single
 * WebAssembly.Memory is created by the boot sequence (§16) before the
 * core module is even instantiated. */
Error emulator_create(Emulator *out);

/* As emulator_create, with an explicit CPU backend instead of the
 * configure-time one (tests run the interpreter under every preset). */
Error emulator_create_with_backend(Emulator *out, const CPU_Backend *backend);
void emulator_destroy(Emulator *emulator);

/* "Load a game" (§12): parses the decrypted PROGRAM NCA in `nca` (§1.6:
 * pre-decrypted only; RESULT_ENCRYPTED_INPUT otherwise, message naming
 * docs/DUMP.md), bootstraps the process (process.h) and arms `cpu_state`
 * with the main thread's entry state. `aslr_seed` 0 disables ASLR. The
 * source must stay readable for the duration of the call only: every
 * byte the guest needs is in guest RAM afterwards (RomFS access is
 * fsp-srv's business, Phase 4, and re-opens the NCA). One program per
 * Emulator: a second call fails with RESULT_INVALID_ARGUMENT until
 * emulator_unload_program(). */
Error emulator_load_program(Emulator *emulator, const Byte_Source *nca, uint64_t aslr_seed);

/* "Load homebrew" (§12): an NRO file (nro.h) becomes the process's single
 * `main` module, with a synthesized npdm (39-bit address space, priority
 * 44, core 0, 1MB stack, program id EMULATOR_HOMEBREW_PROGRAM_ID). Entered
 * with the Horizon ABI (X0 = 0, X1 = main thread handle). */
#define EMULATOR_HOMEBREW_PROGRAM_ID 0x0500000000000001ull
Error emulator_load_nro(Emulator *emulator, const Byte_Source *nro, uint64_t aslr_seed);

/* Loads whichever executable `source` is, decided structurally: an "NRO0"
 * magic at 0x10 is homebrew (emulator_load_nro); anything else is treated
 * as a PROGRAM NCA (emulator_load_program), whose own checks report
 * encrypted or foreign input (§1.6). */
Error emulator_load(Emulator *emulator, const Byte_Source *source, uint64_t aslr_seed);

/* Unmaps the process and resets the page allocator. No-op if nothing is
 * loaded. */
void emulator_unload_program(Emulator *emulator);

/* One scheduler slice (§7): the highest-priority runnable guest thread
 * runs for at most `cycle_budget` cycles. */
Emulator_Status emulator_run_slice(Emulator *emulator, uint64_t cycle_budget);

/* Compatibility wrapper: one slice, reported as the backend's exit reason
 * (CPU_EXIT_HALT when no thread ran). With nothing loaded it runs the
 * bare CPU_State, as before the scheduler existed. */
CPU_ExitReason emulator_run(Emulator *emulator, uint64_t cycle_budget);

/* Routes svcOutputDebugString text to the platform. */
void emulator_set_debug_output(Emulator *emulator, HLE_Debug_Output_Fn fn, void *userdata);

/* Single-step. */
CPU_ExitReason emulator_step(Emulator *emulator);

/* Diagnostics. */
const char *emulator_backend_name(const Emulator *emulator);

#endif /* SWITCH_EMULATOR_H */
