# Builds the platform-agnostic C11 emulator core. See docs/DESIGN.md section 2.

set(SWITCH_CORE_SOURCES
  common/log.c
  common/arena.c
  common/layout.c
  common/vmm.c
  common/input_region.c
  common/workers.c
  cpu/cpu.c
  cpu/backends/noop/noop.c
  cpu/backends/interpreter/interpreter.c
  cpu/backends/interpreter/interp_dp_immediate.c
  cpu/backends/interpreter/interp_branch_system.c
  cpu/backends/interpreter/interp_load_store.c
  cpu/backends/interpreter/interp_dp_register.c
  cpu/backends/interpreter/interp_simd_fp.c
  cpu/backends/interpreter/interp_simd.c
  cpu/backends/interpreter/interp_simd_ldst.c
  cpu/backends/interpreter/interp_predecode.c
  cpu/backends/interpreter/softfloat.c
  cpu/backends/jit/jit.c
  cpu/backends/jit/jit_compile.c
  hle/hle.c
  hle/kernel/page_allocator.c
  hle/kernel/address_space.c
  hle/kernel/process.c
  hle/kernel/svc_memory.c
  hle/kernel/tls.c
  hle/kernel/thread.c
  hle/kernel/handle_table.c
  hle/kernel/ipc.c
  hle/kernel/svc_ipc.c
  hle/kernel/scheduler.c
  hle/kernel/parallel.c
  hle/kernel/event.c
  hle/kernel/shared_memory.c
  hle/services/hid/hid.c
  hle/services/am/am.c
  hle/services/apm/apm.c
  hle/services/set/set.c
  hle/services/time/time.c
  hle/services/fs/fs.c
  hle/services/vi/vi.c
  hle/services/network/network.c
  hle/services/pl/pl.c
  hle/services/misc/misc.c
  hle/services/audio/audout.c
  hle/services/audio/audren.c
  hle/services/acc/acc.c
  hle/services/ns/ns.c
  hle/services/acc/acc_icon.c
  audio/audio_ring.c
  gpu/block_linear.c
  gpu/gpu_channel.c
  gpu/maxwell_shader.c
  gpu/texture.c
  gpu/astc.c
  gpu/raster3d.c
  gpu/gpu_stream.c
  gpu/wgsl.c
  hle/fs/ramfs.c
  hle/fs/save_archive.c
  hle/kernel/svc_thread.c
  hle/services/sm/sm.c
  hle/services/nvdrv/nvdrv.c
  hle/services/nvdrv/nvdec.c
  video/host1x.c
  video/h264.c
  video/video_stream.c
  video/vic_convert.c
  hle/loader/byte_source.c
  hle/loader/nca_parse.c
  hle/loader/nca_compressed.c
  hle/loader/exefs.c
  hle/loader/romfs.c
  hle/loader/npdm.c
  hle/loader/nso.c
  hle/loader/nro.c
  gpu/framebuffer.c
  gpu/syncpoint.c
  emulator.c
)

#
# The ONLY third-party code in core/: single-file LZ4 (BSD-2) for NSO
# segment decompression (docs/DESIGN.md �12, �26). Built with every
# allocating entry point compiled out, so the object contains no
# malloc/calloc/free - the loader uses LZ4_decompress_safe over arena
# storage and tests use the stack-state LZ4_compress_default.
#
set(SWITCH_CORE_THIRD_PARTY_SOURCES
  third_party/lz4/lz4.c
)
# Upstream lz4.c defines one non-static helper without a prototype
# (LZ4_loadDict_internal); relax -Wmissing-prototypes for that file only.
# Every other project warning stays on and lz4.c is clean under them.
set_source_files_properties(${SWITCH_CORE_THIRD_PARTY_SOURCES} PROPERTIES
  COMPILE_DEFINITIONS "LZ4_STATIC_LINKING_ONLY_DISABLE_MEMORY_ALLOCATION;LZ4_HEAPMODE=0"
  COMPILE_OPTIONS "$<$<C_COMPILER_ID:GNU,Clang>:-Wno-missing-prototypes>"
)

set(SWITCH_CORE_PUBLIC_HEADERS
  common/log.h
  common/arena.h
  common/assert.h
  common/result.h
  common/layout.h
  common/vmm.h
  cpu/cpu.h
  cpu/backends/noop/noop.h
  cpu/backends/interpreter/interpreter.h
  cpu/backends/jit/jit.h
  hle/hle.h
  hle/kernel/page_allocator.h
  hle/kernel/address_space.h
  hle/kernel/process.h
  hle/kernel/svc_memory.h
  hle/kernel/tls.h
  hle/kernel/thread.h
  hle/kernel/handle_table.h
  hle/kernel/ipc.h
  hle/kernel/svc_ipc.h
  hle/kernel/scheduler.h
  hle/kernel/parallel.h
  hle/kernel/event.h
  hle/kernel/svc_thread.h
  hle/services/sm/sm.h
  hle/services/nvdrv/nvdrv.h
  common/input_region.h
  common/workers.h
  hle/loader/byte_source.h
  hle/loader/nca_parse.h
  hle/loader/nca_compressed.h
  hle/loader/exefs.h
  hle/loader/romfs.h
  hle/loader/npdm.h
  hle/loader/nso.h
  hle/loader/nro.h
  gpu/framebuffer.h
  gpu/syncpoint.h
  gpu/maxwell_shader.h
  gpu/texture.h
  gpu/astc.h
  gpu/raster3d.h
  gpu/gpu_stream.h
  emulator.h
)

add_library(switch_core STATIC ${SWITCH_CORE_SOURCES} ${SWITCH_CORE_THIRD_PARTY_SOURCES})

target_include_directories(switch_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

# common/workers: pthreads natively (the web build's -pthread covers it).
if(NOT EMSCRIPTEN AND NOT WIN32)
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
  target_link_libraries(switch_core PUBLIC Threads::Threads)
endif()

#
# Backend selection. Every backend obeys CPU_Backend; a single define wires
# cpu_get_active_backend() to the compiled-in choice. Additional backends
# (interpreter, ballistic) will add their own subdirectory and define.
#
if(CPU_BACKEND STREQUAL "noop")
  target_compile_definitions(switch_core PUBLIC SWITCH_CPU_BACKEND_NOOP=1)
elseif(CPU_BACKEND STREQUAL "interpreter")
  # The interpreter is always compiled (its tests run under every preset);
  # this only makes it the active backend.
  target_compile_definitions(switch_core PUBLIC SWITCH_CPU_BACKEND_INTERPRETER=1)
elseif(CPU_BACKEND STREQUAL "jit")
  # Voland's own ARM64 -> wasm JIT (docs/JIT.md); mixed mode over the
  # interpreter, which is why the interpreter is always compiled too.
  target_compile_definitions(switch_core PUBLIC SWITCH_CPU_BACKEND_JIT=1)
elseif(CPU_BACKEND STREQUAL "ballistic")
  message(FATAL_ERROR "ballistic backend is a Phase 5 deliverable, not yet built (§10)")
else()
  message(FATAL_ERROR "Unknown CPU_BACKEND '${CPU_BACKEND}'")
endif()

#
# Web build: expose the core as a WASM module directly from this target.
#
if(EMSCRIPTEN)
  add_executable(switch_core_wasm stubs/wasm_entry.c)
  target_link_libraries(switch_core_wasm PRIVATE switch_core)
  set_target_properties(switch_core_wasm PROPERTIES
    OUTPUT_NAME "switch_core"
    SUFFIX ".js"
  )
  # The CPU worker imports the module from /core/switch_core.js
  # (platform/web/workers/cpu.worker.ts); Vite serves platform/web/public/
  # verbatim, so stage both artifacts there after every link. The staged
  # copies are build output and gitignored.
  set(VOLAND_WEB_CORE_DIR ${PROJECT_SOURCE_DIR}/platform/web/public/core)
  add_custom_command(TARGET switch_core_wasm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${VOLAND_WEB_CORE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE_DIR:switch_core_wasm>/switch_core.js
            $<TARGET_FILE_DIR:switch_core_wasm>/switch_core.wasm
            ${VOLAND_WEB_CORE_DIR}
    COMMENT "Staging switch_core.{js,wasm} into platform/web/public/core"
  )
endif()
