/**
 * Thin Emscripten entry point. Exposes the emulator public API as exported
 * C functions so the CPU worker can call into the core over ccall/cwrap.
 * See docs/DESIGN.md section 24 (WASM build flags, EXPORTED_FUNCTIONS).
 *
 * Wrappers are `_ffi` suffixed rather than the bare core function names:
 * `emulator_create`/`emulator_run`/etc. already exist as real functions
 * taking an `Emulator*` the JS side has no way to name, so each wrapper
 * here operates on the single module-global instance instead. `layout_get`
 * needs no wrapper since it already takes no arguments.
 */
#include "common/layout.h"
#include "common/log.h"
#include "emulator.h"
#include "gpu/framebuffer.h"
#include "hle/loader/byte_source.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

/* Prototypes for -Wmissing-prototypes: these are called from JS, never
 * from another C translation unit, so there is nowhere sensible to put a
 * shared header - but the project's own strict-warnings policy (§3) still
 * requires one. */
EXPORT int emulator_create_ffi(void);
EXPORT void emulator_destroy_ffi(void);
EXPORT int emulator_run_ffi(uint64_t cycle_budget);
EXPORT int emulator_step_ffi(void);
EXPORT uint64_t cpu_get_reg_ffi(uint32_t index);
EXPORT void cpu_set_reg_ffi(uint32_t index, uint64_t value);
EXPORT uint64_t cpu_get_pc_ffi(void);
EXPORT int cpu_backend_id_ffi(void);
EXPORT uint64_t layout_get_ffi(void);
EXPORT int emulator_load_program_ffi(uint64_t file_size, uint64_t aslr_seed);
EXPORT void emulator_unload_program_ffi(void);
EXPORT uint64_t emulator_last_error_message_ffi(void);
EXPORT uint64_t emulator_program_id_ffi(void);
EXPORT int emulator_run_slice_ffi(uint64_t cycle_budget);
EXPORT uint64_t emulator_virtual_ticks_ffi(void);
EXPORT uint64_t emulator_crash_pc_ffi(void);

/* The Switch's handheld resolution. */
#define BOOT_FRAME_WIDTH 1280u
#define BOOT_FRAME_HEIGHT 720u

static Emulator g_emulator;
static int g_initialised = 0;

/* Message of the most recent failed emulator_load_program_ffi call. Always
 * a static string (Error.message is never heap allocated, §3), so handing
 * its address to JS is safe for the life of the module. */
static const char *g_last_error_message = NULL;

/* Host read hook for the game file (byte_source.h contract: synchronous,
 * exactly `size` bytes or failure). The CPU worker supplies
 * `volandReadGameFile` through the module factory options; it reads the
 * user's File with FileReaderSync over `blob.slice()` (§15) straight into
 * linear memory at `out`. Returns 1 on a full read, 0 otherwise. Pointers
 * and u64 values may arrive as BigInt under wasm64, so the JS side
 * normalizes both with Number() - offsets into a File and into linear
 * memory are both below 2^53. */
/* Guest svcOutputDebugString text -> the CPU worker (Module.volandGuestOutput),
 * which relays it to the page. Diagnostic text at guest pace, not per-frame
 * data (§6). */
#ifdef __EMSCRIPTEN__
EM_JS(void, voland_host_guest_output, (const char *text, uint64_t length), {
  const sink = Module["volandGuestOutput"];
  if (typeof sink === "function") sink(Number(text), Number(length));
})
#else
static void voland_host_guest_output(const char *text, uint64_t length)
{
  (void)text;
  (void)length;
}
#endif

static void forward_guest_output(void *userdata, const char *text, size_t length)
{
  (void)userdata;
  voland_host_guest_output(text, (uint64_t)length);
}

#ifdef __EMSCRIPTEN__
EM_JS(int, voland_host_read_game_file, (uint64_t offset, void *out, uint64_t size), {
  const read = Module["volandReadGameFile"];
  if (typeof read !== "function") return 0;
  return read(Number(offset), Number(out), Number(size)) ? 1 : 0;
})
#else
static int voland_host_read_game_file(uint64_t offset, void *out, uint64_t size)
{
  (void)offset;
  (void)out;
  (void)size;
  return 0; /* the native smoke binary never loads through the FFI */
}
#endif

static Error host_game_file_read(void *user, uint64_t offset, void *out, uint64_t size)
{
  (void)user;
  if (!voland_host_read_game_file(offset, out, size))
    return ERR(RESULT_IO_ERROR, "host read of the game file failed");
  return OK;
}

EXPORT int emulator_create_ffi(void)
{
  if (g_initialised)
    return 1;
  const Error err = emulator_create(&g_emulator);
  if (err.code != RESULT_OK)
  {
    log_error("[wasm_entry] emulator_create failed: %s", err.message);
    return 0;
  }
  emulator_set_debug_output(&g_emulator, forward_guest_output, NULL);
  /* The display shows Voland's test card until a guest presents (§6). */
  framebuffer_reset();
  (void)framebuffer_publish_test_card(BOOT_FRAME_WIDTH, BOOT_FRAME_HEIGHT);
  g_initialised = 1;
  return 1;
}

EXPORT void emulator_destroy_ffi(void)
{
  if (!g_initialised)
    return;
  emulator_destroy(&g_emulator);
  g_initialised = 0;
}

/* Returns CPU_ExitReason as an int; scheduler_tick (§7) replaces this
 * caller once Phase 2 lands. */
EXPORT int emulator_run_ffi(uint64_t cycle_budget)
{
  if (!g_initialised)
    return -1;
  return (int)emulator_run(&g_emulator, cycle_budget);
}

EXPORT int emulator_step_ffi(void)
{
  if (!g_initialised)
    return -1;
  return (int)emulator_step(&g_emulator);
}

EXPORT uint64_t cpu_get_reg_ffi(uint32_t index)
{
  if (!g_initialised)
    return 0;
  return g_emulator.cpu_backend->get_reg(g_emulator.cpu_state, (uint8_t)index);
}

EXPORT void cpu_set_reg_ffi(uint32_t index, uint64_t value)
{
  if (!g_initialised)
    return;
  g_emulator.cpu_backend->set_reg(g_emulator.cpu_state, (uint8_t)index, value);
}

EXPORT uint64_t cpu_get_pc_ffi(void)
{
  if (!g_initialised)
    return 0;
  return g_emulator.cpu_backend->get_pc(g_emulator.cpu_state);
}

/* Numeric backend id rather than marshalling `name`/`version` C strings
 * across the FFI boundary - platform/web/bindings/core.ts keeps the
 * matching display-name table. Sidesteps wasm64 string-pointer
 * marshalling entirely for a value that is fixed at compile time anyway
 * (CPU_BACKEND is a CMake configure-time choice, never a runtime one). */
EXPORT int cpu_backend_id_ffi(void)
{
#if defined(SWITCH_CPU_BACKEND_NOOP)
  return 0; /* CpuBackendId.Noop, bindings/core.ts */
#elif defined(SWITCH_CPU_BACKEND_INTERPRETER)
  return 1; /* CpuBackendId.Interpreter */
#else
  return -1;
#endif
}

/* Returns a linear-memory pointer to the live Memory_Layout struct (§4) as
 * a plain integer, cast from the module's own address space. platform/web
 * bindings/layout.ts mirrors the C struct field-for-field and reads it
 * back out of the shared WebAssembly.Memory at this offset. */
EXPORT uint64_t layout_get_ffi(void)
{
  const Memory_Layout *layout = layout_get();
  return (uint64_t)(uintptr_t)layout;
}

/* "Load a game" over the FFI (§12 process bootstrap). The game file
 * itself never crosses into linear memory as a whole: the core reads it
 * piecewise through voland_host_read_game_file (§15). Returns the
 * core's Result code (common/result.h; 0 = RESULT_OK). On failure the
 * message is available from emulator_last_error_message_ffi - in
 * particular RESULT_ENCRYPTED_INPUT, which the web UI turns into the
 * dumping-guide error (§1.6). */
EXPORT int emulator_load_program_ffi(uint64_t file_size, uint64_t aslr_seed)
{
  if (!g_initialised)
  {
    g_last_error_message = "emulator is not initialised";
    return (int)RESULT_INVALID_ARGUMENT;
  }
  const Byte_Source source = {
      .user = NULL,
      .size = file_size,
      .read = host_game_file_read,
  };
  const Error err = emulator_load(&g_emulator, &source, aslr_seed);
  g_last_error_message = err.message;
  if (err.code != RESULT_OK)
    log_warn("[wasm_entry] emulator_load_program failed (%d): %s", (int)err.code,
             err.message ? err.message : "(no message)");
  return (int)err.code;
}

EXPORT void emulator_unload_program_ffi(void)
{
  if (!g_initialised)
    return;
  emulator_unload_program(&g_emulator);
}

/* One scheduler slice (§7); returns Emulator_Status. The CPU worker's
 * loop body. */
EXPORT int emulator_run_slice_ffi(uint64_t cycle_budget)
{
  if (!g_initialised)
    return (int)EMULATOR_NOT_LOADED;
  return (int)emulator_run_slice(&g_emulator, cycle_budget);
}

EXPORT uint64_t emulator_virtual_ticks_ffi(void)
{
  return g_initialised ? g_emulator.scheduler.ticks : 0;
}

EXPORT uint64_t emulator_crash_pc_ffi(void)
{
  return g_initialised ? g_emulator.scheduler.crash_pc : 0;
}

/* Linear-memory address of a NUL-terminated static string, or 0. */
EXPORT uint64_t emulator_last_error_message_ffi(void)
{
  return (uint64_t)(uintptr_t)g_last_error_message;
}

/* The loaded program's id from main.npdm's ACI0, or 0 if nothing is
 * loaded. Shown to the user as the 16-hex-digit title id. */
EXPORT uint64_t emulator_program_id_ffi(void)
{
  if (!g_initialised || !g_emulator.program_loaded)
    return 0;
  return g_emulator.process.npdm.program_id;
}

#ifndef __EMSCRIPTEN__
int main(void)
{
  log_info("[wasm_entry] native build, running smoke test");
  Emulator emu;
  const Error err = emulator_create(&emu);
  if (err.code != RESULT_OK)
  {
    log_error("emulator_create failed: %s", err.message);
    return 1;
  }
  const CPU_ExitReason reason = emulator_run(&emu, 1000);
  log_info("[wasm_entry] emulator_run exit reason=%d", (int)reason);
  emulator_destroy(&emu);
  return 0;
}
#endif
