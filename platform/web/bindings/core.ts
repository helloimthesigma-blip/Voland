/**
 * Typed surface of the WASM core's exported functions. The actual module
 * is produced by `emcmake cmake ... && cmake --build build`, which emits
 * `switch_core.js` + `switch_core.wasm` (see CMakeLists.txt EXPORTED_
 * FUNCTIONS, mirrored 1:1 below). Every export is `_ffi` suffixed because
 * the real core functions take an `Emulator*` the JS side has no way to
 * name; each wrapper operates on the module-global instance instead.
 *
 * wasm64 (`-m64`, §24) means every pointer-or-uint64-typed export
 * exchanges `bigint`, never `number`, at the WASM boundary - i64 crosses
 * the JS boundary as BigInt, which is mandatory under wasm64.
 */

export interface SwitchCoreExports {
  readonly _emulator_create_ffi:  () => number;
  readonly _emulator_destroy_ffi: () => void;
  readonly _emulator_run_ffi:     (cycleBudget: bigint) => number; /* CPU_ExitReason */
  readonly _emulator_step_ffi:    () => number;                    /* CPU_ExitReason */

  readonly _layout_get_ffi:      () => bigint; /* Memory_Layout* in linear memory */
  readonly _cpu_backend_id_ffi:  () => number; /* CpuBackendId */

  readonly _cpu_get_reg_ffi: (index: number) => bigint;
  readonly _cpu_set_reg_ffi: (index: number, value: bigint) => void;
  readonly _cpu_get_pc_ffi:  () => bigint;

  /* Game load (core/stubs/wasm_entry.c). The file is never copied in
   * whole: the core pulls bytes through the `volandReadGameFile` hook the
   * worker passes to the module factory (§15). Returns a Result code
   * (bindings/load.ts CoreResult). */
  readonly _emulator_load_program_ffi:        (fileSize: bigint, aslrSeed: bigint) => number;
  readonly _emulator_unload_program_ffi:      () => void;
  readonly _emulator_last_error_message_ffi:  () => bigint; /* const char* or 0 */
  readonly _emulator_program_id_ffi:          () => bigint;

  /* Execution (§7): one scheduler slice; returns EmulatorStatus. */
  readonly _emulator_run_slice_ffi:     (cycleBudget: bigint) => number;
  readonly _emulator_virtual_ticks_ffi: () => bigint;
  readonly _emulator_crash_pc_ffi:      () => bigint;
  /* Perf counters (wasm_entry.c): writes up to `capacity` uint64s at
   * `out` (ticks, SVCs, GPU stream bytes, GPU stream stalls, stream wait ns); returns the count. */
  readonly _emulator_perf_counters_ffi: (out: bigint, capacity: number) => number;

  /* Homebrew and system setup (v3.41). Strings and bytes live in memory
   * from `_malloc`; the shared font's allocation is never freed. */
  readonly _emulator_set_program_path_ffi: (path: bigint) => void;
  readonly _emulator_set_rtc_ffi:          (unixSeconds: bigint) => void;
  readonly _emulator_set_frame_skip_ffi:   (frames: number) => void;
  readonly _emulator_recent_problems_ffi: () => bigint;
  /* Compatibility report: distinct problems text, renderer counters (u64[10]). */
  readonly _emulator_distinct_problems_ffi: () => bigint;
  readonly _emulator_render_stats_ffi: () => bigint;
  readonly _emulator_set_gpu_mode_ffi:     (on: number) => void;
  readonly _emulator_set_host_cores_ffi:   (cores: number) => number; /* cores in effect */
  readonly _emulator_set_gpu_async_ffi:    (on: number) => number;    /* 1: asynchronous GPU on */
  readonly _emulator_set_render_workers_ffi: (count: number) => number; /* workers in effect */
  readonly _emulator_dump_threads_ffi:     (backtrace: number) => void;
  readonly _emulator_set_gpu_compute_ffi:  (on: number) => void;
  readonly _emulator_set_poll_coalescing_ffi: (on: number) => void;
  readonly _emulator_set_free_running_ffi: (on: number) => void;
  readonly _emulator_set_pacing_ffi:       (on: number) => void;
  readonly _emulator_pacing_resync_ffi:    () => void;
  readonly _emulator_run_for_ffi:          (hostMs: number, cycleBudget: bigint) => number; /* Emulator_Status */
  readonly _emulator_set_shared_font_ffi:  (bytes: bigint, size: number) => void;
  readonly _emulator_sd_write_file_ffi:    (path: bigint, bytes: bigint, size: bigint) => number; /* Result */
  readonly _emulator_sd_clear_ffi:         () => number; /* Result */
  /* Guest-write mirroring (v3.50): the filesystem change counter, the SD
   * manifest ("version size path\n" lines; returns the bytes needed) and
   * a file read (returns the file's size, or -1). */
  readonly _emulator_sd_generation_ffi: () => number;
  readonly _emulator_sd_manifest_ffi:   (out: bigint, max: number) => number;
  readonly _emulator_sd_read_file_ffi:  (path: bigint, out: bigint, max: number) => number;
  /* Committed save archives (§15; core/hle/fs/save_archive.h). */
  readonly _emulator_save_commits_ffi: () => number;
  readonly _emulator_save_committed_manifest_ffi: (out: bigint, max: number) => number;
  readonly _emulator_save_restore_archive_ffi: (name: bigint, bytes: bigint, size: number) => number;
  /* Save states (core/emulator.h; workers/savestate.ts): the plan's address or 0. */
  readonly _emulator_savestate_begin_save_ffi: () => number;
  readonly _emulator_savestate_end_save_ffi: () => void;
  readonly _emulator_savestate_begin_restore_ffi: () => number;
  readonly _emulator_savestate_finish_restore_ffi: (applied: number) => number;
  /* Software keyboard (v3.54): writes the pending request's fields joined
   * by U+001F and returns 1 (0 = none); respond with UTF-8 text. */
  readonly _emulator_text_request_ffi: (out: bigint, max: number) => number;
  readonly _emulator_text_respond_ffi: (text: bigint, accepted: number) => void;
  /* Emscripten's own wrappers: under MEMORY64 they exchange pointers
   * as JS numbers (unlike the uint64_t parameters above). */
  readonly _malloc: (size: number) => number;
  readonly _free:   (pointer: number) => void;
}

/* Guest svcOutputDebugString hook: text at `address` in linear memory. */
export type GuestOutputHook = (address: number, length: number) => void;

/**
 * Synchronous read hook the core calls while loading (byte_source.h
 * contract): copy exactly `size` bytes of the game file at `offset` into
 * linear memory at `destination`, returning false on any failure.
 */
export type GameFileReadHook = (offset: number, destination: number, size: number) => boolean;

/* Mirrors the #if ladder in core/stubs/wasm_entry.c's cpu_backend_id_ffi -
 * CPU_BACKEND is a CMake configure-time choice, never a runtime one, so
 * this sidesteps marshalling the backend's `name`/`version` C strings
 * across the wasm64 FFI boundary for a value fixed at build time. */
export const enum CpuBackendId {
  Noop = 0,
  Interpreter = 1,
  Ballistic = 2,
  Jit = 3,
}

export const CPU_BACKEND_DISPLAY_NAMES: Readonly<Record<number, string>> = {
  [CpuBackendId.Noop]:        "noop",
  [CpuBackendId.Interpreter]: "interpreter",
  [CpuBackendId.Ballistic]:   "ballistic",
  [CpuBackendId.Jit]:         "jit",
};

/* CPU_ExitReason, mirrors core/cpu/cpu.h. */
export const enum CpuExitReason {
  CyclesElapsed = 0,
  Svc           = 1,
  Halt          = 2,
  Breakpoint    = 3,
  Fault         = 4,
}

/* ARM64 general-purpose register indices. Mirrors core/cpu/cpu.h.
 * Index 31 does not exist at this interface - there is no XZR/SP constant
 * here; SP has its own accessor and XZR is not architectural state. */
export const CPU_REG_X0  = 0;
export const CPU_REG_X29 = 29;
export const CPU_REG_X30 = 30;
