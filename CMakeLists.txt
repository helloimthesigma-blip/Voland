cmake_minimum_required(VERSION 3.24)
project(Voland
  VERSION 0.0.1
  DESCRIPTION "Nintendo Switch emulator, WASM-first C core"
  LANGUAGES C)

#
# Options (see docs/DESIGN.md section 24)
#
set(CPU_BACKEND "noop" CACHE STRING "CPU backend: noop|interpreter|jit|ballistic")
set_property(CACHE CPU_BACKEND PROPERTY STRINGS noop interpreter jit ballistic)

option(VOLAND_WASM_PROFILING "Web: keep wasm function names (--profiling-funcs) for browser profiles" OFF)

set(GPU_BACKEND "auto" CACHE STRING "GPU backend: webgpu|vulkan|metal|d3d12|auto")
set_property(CACHE GPU_BACKEND PROPERTY STRINGS webgpu vulkan metal d3d12 auto)

set(AUDIO_BACKEND "auto" CACHE STRING "Audio backend: wasapi|coreaudio|pipewire|none|auto (web: none, §14)")
set_property(CACHE AUDIO_BACKEND PROPERTY STRINGS wasapi coreaudio pipewire none auto)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Debug|Release|RelWithDebInfo" FORCE)
endif()

#
# Language settings. Core is C11 strict, no C++ anywhere in core (§3).
#
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#
# Strict warnings on the core. No exceptions-based error handling.
#
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
                      -Wmissing-prototypes -Wno-unused-parameter)
endif()

message(STATUS "Voland configuration")
message(STATUS "  CPU_BACKEND:   ${CPU_BACKEND}")
message(STATUS "  GPU_BACKEND:   ${GPU_BACKEND}")
message(STATUS "  AUDIO_BACKEND: ${AUDIO_BACKEND}")
message(STATUS "  Build type:    ${CMAKE_BUILD_TYPE}")

#
# WASM / Emscripten build flags (see docs/DESIGN.md section 24).
#
# The core module owns no memory of its own: the boot sequence (§16)
# creates ONE shared, memory64 WebAssembly.Memory and imports it in, sized
# to fit guest RAM + every layout.h region + Emscripten's own data/stack/
# heap. INITIAL_MEMORY here and TOTAL_MEMORY_PAGES in platform/web/src/
# main.ts must both equal 5_637_144_576 bytes (~5.25GiB) - growth is
# disabled, so a mismatch is an instantiation-time failure, not silent
# truncation.
#
if(EMSCRIPTEN)
  # Minimum toolchain (mirrors global.json). The flag spellings below are
  # verified against 6.0.9; refuse older SDKs here rather than let them fail
  # later with an opaque unknown-flag or wasm32/wasm64 link error.
  set(VOLAND_MIN_EMSCRIPTEN_VERSION 6.0.9)
  if(EMSCRIPTEN_VERSION VERSION_LESS VOLAND_MIN_EMSCRIPTEN_VERSION)
    message(FATAL_ERROR
      "Emscripten ${EMSCRIPTEN_VERSION} is too old; Voland requires "
      ">= ${VOLAND_MIN_EMSCRIPTEN_VERSION} (see global.json). "
      "Run: emsdk install latest && emsdk activate latest")
  endif()

  # wasm64/pthreads change codegen (wasm64 target, shared-memory atomics),
  # not just link behavior - they are REQUIRED on both the compile and link
  # lines, or object files come out wasm32 and linking fails outright
  # ("wasm32 object file can't be linked in wasm64 mode").
  #
  # Spellings are the ones emcc 6.0.9 accepts without -Wdeprecated:
  # -m64 (not -sMEMORY64=1) and -pthread (not -sUSE_PTHREADS=1). Both are
  # the standard Clang flags; the -s forms were emcc-only aliases.
  set(VOLAND_WASM_CODEGEN_FLAGS
    -msimd128                 # REQUIRED: NEON -> WASM SIMD; ~4x slowdown without
    -m64                       # wasm64: REQUIRED for Switch 1, not just Switch 2 (§4)
    -pthread                   # implies SHARED_MEMORY (shared-memory atomics)
    -sSHARED_MEMORY=1          # redundant under -pthread; kept for explicitness
  )
  add_compile_options(${VOLAND_WASM_CODEGEN_FLAGS})
  add_link_options(
    ${VOLAND_WASM_CODEGEN_FLAGS}
    -sWASM=1
    -sIMPORTED_MEMORY=1        # host creates the single WebAssembly.Memory (§16)
    -sINITIAL_MEMORY=5637144576   # ~5.25GB; must equal the host's initial; with
                                  # growth disabled this IS the maximum, so no
                                  # separate MAXIMUM_MEMORY - views never detach
    -sALLOW_MEMORY_GROWTH=0       # fixed at boot (§4); never enable
    -sSTACK_SIZE=1048576          # 1 MiB (Emscripten's default is 64 KiB): the
    -sDEFAULT_PTHREAD_STACK_SIZE=1048576 # WGSL translator under a draw under an
                                  # SVC under the JIT's run loop overflowed 64 KiB
                                  # and silently corrupted dlmalloc's static
                                  # state (heap corruption, bogus 8 GiB sbrk).
                                  # Memory stays fixed: this only moves the heap.
    -sALLOW_TABLE_GROWTH=1        # the JIT installs compiled blocks with
                                  # addFunction (docs/JIT.md); the function
                                  # table grows, linear memory never does
    -sPTHREAD_POOL_SIZE=8
    -sMODULARIZE=1             # emits a factory function, not a self-executing script
    -sEXPORT_ES6=1             # ...as an ES module, so cpu.worker.ts can `import()` it
    -sEXPORT_NAME=createVolandCoreModule
    -sENVIRONMENT=worker       # only ever instantiated inside cpu.worker.ts
    "-sEXPORTED_FUNCTIONS=['_emulator_create_ffi','_emulator_destroy_ffi',
                            '_emulator_run_ffi','_emulator_step_ffi',
                            '_layout_get_ffi','_cpu_backend_id_ffi',
                            '_cpu_get_reg_ffi','_cpu_set_reg_ffi','_cpu_get_pc_ffi',
                            '_emulator_load_program_ffi','_emulator_unload_program_ffi',
                            '_emulator_last_error_message_ffi','_emulator_program_id_ffi',
                            '_emulator_run_slice_ffi','_emulator_virtual_ticks_ffi','_emulator_perf_counters_ffi','_emulator_crash_pc_ffi','_emulator_recent_problems_ffi','_emulator_distinct_problems_ffi','_emulator_render_stats_ffi',
                            '_emulator_set_program_path_ffi','_emulator_set_rtc_ffi','_emulator_set_frame_skip_ffi','_emulator_set_gpu_mode_ffi','_emulator_set_host_cores_ffi','_emulator_set_poll_coalescing_ffi','_emulator_set_free_running_ffi','_emulator_run_for_ffi','_emulator_set_pacing_ffi','_emulator_pacing_resync_ffi',
                            '_emulator_set_shared_font_ffi','_emulator_sd_write_file_ffi','_emulator_sd_clear_ffi',
                            '_emulator_sd_generation_ffi','_emulator_sd_manifest_ffi','_emulator_sd_read_file_ffi',
                            '_emulator_save_commits_ffi','_emulator_save_committed_manifest_ffi','_emulator_save_restore_archive_ffi',
                            '_emulator_savestate_begin_save_ffi','_emulator_savestate_end_save_ffi',
                            '_emulator_savestate_begin_restore_ffi','_emulator_savestate_finish_restore_ffi',
                            '_emulator_text_request_ffi','_emulator_text_respond_ffi',
                            '_malloc','_free']"
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
  )
  if(VOLAND_WASM_PROFILING)
    add_link_options(--profiling-funcs)  # names only; same optimised code
  endif()
endif()

add_subdirectory(core)

# voland-cli (§17) builds on every native platform.
if(NOT EMSCRIPTEN)
  add_subdirectory(platform/cli)
endif()

#
# Optional: tests (Phase 0 ships with a smoke test only)
#
option(SWITCH_EMU_BUILD_TESTS "Build test binaries" ON)
if(SWITCH_EMU_BUILD_TESTS AND NOT EMSCRIPTEN)
  enable_testing()
  add_subdirectory(tests)
elseif(SWITCH_EMU_BUILD_TESTS AND EMSCRIPTEN AND CPU_BACKEND STREQUAL "jit")
  # Wasm-side JIT tests, run under Node: ctest --preset web-jit
  enable_testing()
  add_subdirectory(tests/jit_wasm)
endif()
if(SWITCH_EMU_BUILD_TESTS AND EMSCRIPTEN)
  # Parallel guest threads under Emscripten pthreads, run by Node; built
  # only on request (tests/parallel_wasm/CMakeLists.txt).
  enable_testing()
  add_subdirectory(tests/parallel_wasm)
endif()
