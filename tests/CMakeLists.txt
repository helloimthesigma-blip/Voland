# smoke_phase0 is the cross-subsystem integration path (emulator/cpu/hle/
# vmm wired together); layout_test and vmm_test are common/layout's (§4)
# and common/vmm's (§5) own unit tests. Further per-subsystem tests land
# alongside the subsystems they cover (CLAUDE.md: "every C subsystem
# ships tests in tests/").

add_executable(smoke_phase0 smoke_phase0.c)
target_link_libraries(smoke_phase0 PRIVATE switch_core)
add_test(NAME smoke_phase0 COMMAND smoke_phase0)

add_executable(layout_test layout_test.c)
target_link_libraries(layout_test PRIVATE switch_core)
add_test(NAME layout_test COMMAND layout_test)

add_executable(vmm_test vmm_test.c)
target_link_libraries(vmm_test PRIVATE switch_core)
add_test(NAME vmm_test COMMAND vmm_test)

#
# core/hle/loader (§1.6, §12): one unit test per parser over synthesized
# images from loader_fixtures.c, plus loader_smoke for the composed
# bootstrap path (NCA -> ExeFS -> npdm, NCA -> ExeFS -> NSO -> segment,
# NCA -> RomFS -> file).
#
add_library(loader_fixtures STATIC loader_fixtures.c)
target_include_directories(loader_fixtures PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# NSO fixtures compress segments with the vendored LZ4 (core/third_party).
target_link_libraries(loader_fixtures PRIVATE switch_core)

foreach(loader_test byte_source_test exefs_test romfs_test npdm_test nca_parse_test nso_test loader_smoke)
  add_executable(${loader_test} ${loader_test}.c)
  target_link_libraries(${loader_test} PRIVATE switch_core loader_fixtures)
  add_test(NAME ${loader_test} COMMAND ${loader_test})
endforeach()

# Writes the synthetic PROGRAM NCA the browser e2e loads (platform/web/
# e2e/load.spec.ts) and proves it loads from disk. The path is fixed so
# the e2e can find it: build/native-noop/fixtures/synthetic_program.nca.
add_executable(program_nca_fixture program_nca_fixture.c)
target_link_libraries(program_nca_fixture PRIVATE switch_core loader_fixtures)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fixtures)
add_test(NAME program_nca_fixture
         COMMAND program_nca_fixture ${CMAKE_BINARY_DIR}/fixtures/synthetic_program.nca)

#
# core/hle/kernel (§12 process bootstrap): the page allocator and address
# space are pure units; process_test drives the bootstrap over a
# synthesized ExeFS and asserts through vmm.
#
add_executable(page_allocator_test page_allocator_test.c)
target_link_libraries(page_allocator_test PRIVATE switch_core)
add_test(NAME page_allocator_test COMMAND page_allocator_test)

add_executable(address_space_test address_space_test.c)
target_link_libraries(address_space_test PRIVATE switch_core)
add_test(NAME address_space_test COMMAND address_space_test)

add_executable(process_test process_test.c)
target_link_libraries(process_test PRIVATE switch_core loader_fixtures)
add_test(NAME process_test COMMAND process_test)

#
# core/hle/kernel TLS + threads (§12 "TLS is the transport"): tls_test
# drives the block allocator over a real vmm; thread_test arms threads
# through the noop backend and asserts the register file and
# tpidrro_el0 through the backend interface.
#
add_executable(tls_test tls_test.c)
target_link_libraries(tls_test PRIVATE switch_core)
add_test(NAME tls_test COMMAND tls_test)

add_executable(thread_test thread_test.c)
target_link_libraries(thread_test PRIVATE switch_core)
add_test(NAME thread_test COMMAND thread_test)

#
# core/hle/kernel/svc_memory (§25 checkbox 5): SVC-path tests over a real
# process_bootstrap()'d Process, driven through hle_on_svc the way
# thread_test drives thread SVCs.
#
add_executable(svc_memory_test svc_memory_test.c)
target_link_libraries(svc_memory_test PRIVATE switch_core loader_fixtures)
add_test(NAME svc_memory_test COMMAND svc_memory_test)

#
# core/hle/kernel IPC (§12 "The call surface"): the handle table as a
# unit; HIPC/CMIF/TIPC parse + encode byte-exact against an independent
# encoder (ipc_fixtures.c restates libnx's layout); and the SVC path -
# ConnectToNamedPort / SendSyncRequest / CloseHandle and sm: - driven
# through hle_on_svc with the command buffer in a real TLS block.
#
add_library(ipc_fixtures STATIC ipc_fixtures.c)
target_include_directories(ipc_fixtures PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ipc_fixtures PRIVATE switch_core loader_fixtures)

add_executable(handle_table_test handle_table_test.c)
target_link_libraries(handle_table_test PRIVATE switch_core)
add_test(NAME handle_table_test COMMAND handle_table_test)

foreach(ipc_test ipc_parse_test svc_ipc_test)
  add_executable(${ipc_test} ${ipc_test}.c)
  target_link_libraries(${ipc_test} PRIVATE switch_core ipc_fixtures loader_fixtures)
  add_test(NAME ${ipc_test} COMMAND ${ipc_test})
endforeach()

#
# core/common/input_region (§18): seqlock reader/writer, byte layout
# against the vectors the web writer's unit test also consumes.
#
find_package(Threads REQUIRED)
add_executable(input_region_test input_region_test.c)
target_link_libraries(input_region_test PRIVATE switch_core Threads::Threads)
add_test(NAME input_region_test
         COMMAND input_region_test ${CMAKE_CURRENT_SOURCE_DIR}/data/input_region_vectors.txt)

#
# core/cpu/backends/interpreter: differential test against the host CPU
# (ARM64 hosts only - the hardware is the oracle; a64_diff_test.c).
#
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
  enable_language(ASM)
  add_executable(a64_diff_test a64_diff_test.c a64_oracle_stub.S)
  target_link_libraries(a64_diff_test PRIVATE switch_core)
  add_test(NAME a64_diff_test COMMAND a64_diff_test 3000)
endif()

# Interpreter behavior the oracle cannot reach (control flow, exceptions,
# exclusives, run loop) - portable, every host.
add_executable(interpreter_test interpreter_test.c)
target_link_libraries(interpreter_test PRIVATE switch_core)
add_test(NAME interpreter_test COMMAND interpreter_test)

#
# Guest threads end to end (§7): real ARM64 programs (tests/guest/*.s,
# assembled into *.inc by tools/guest_asm.py) run on the interpreter
# under the scheduler, booted as real processes by guest_fixture.c.
#
add_library(guest_fixture STATIC guest_fixture.c)
target_include_directories(guest_fixture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(guest_fixture PRIVATE switch_core loader_fixtures)

add_executable(scheduler_test scheduler_test.c)
target_link_libraries(scheduler_test PRIVATE switch_core guest_fixture loader_fixtures)
add_test(NAME scheduler_test COMMAND scheduler_test)

# Homebrew NRO (§12): the committed demo NRO loads and runs end to end.
add_executable(nro_test nro_test.c)
target_link_libraries(nro_test PRIVATE switch_core)
add_test(NAME nro_test COMMAND nro_test ${PROJECT_SOURCE_DIR}/platform/web/public/demo/hello.nro)

add_executable(arena_test arena_test.c)
target_link_libraries(arena_test PRIVATE switch_core)
add_test(NAME arena_test COMMAND arena_test)

# core/gpu/framebuffer (§6 frame handoff).
add_executable(framebuffer_test framebuffer_test.c)
target_link_libraries(framebuffer_test PRIVATE switch_core)
add_test(NAME framebuffer_test COMMAND framebuffer_test)

# core/gpu/syncpoint (§13 syncpoints + the GPU completion ring).
add_executable(syncpoint_test syncpoint_test.c)
target_link_libraries(syncpoint_test PRIVATE switch_core)
add_test(NAME syncpoint_test COMMAND syncpoint_test)

# nvdrv (§12, §13) over real IPC.
add_executable(nvdrv_test nvdrv_test.c)
target_link_libraries(nvdrv_test PRIVATE switch_core ipc_fixtures loader_fixtures)
add_test(NAME nvdrv_test COMMAND nvdrv_test)

# hid + shared memory (§12, §18) over real IPC and SVCs.
add_executable(hid_test hid_test.c)
target_link_libraries(hid_test PRIVATE switch_core ipc_fixtures loader_fixtures)
add_test(NAME hid_test COMMAND hid_test)

#
# voland-cli (§17) as the golden-image / end-to-end harness.
#
set(VOLAND_DEMO_NRO ${PROJECT_SOURCE_DIR}/platform/web/public/demo/hello.nro)
add_test(NAME cli_run_demo
         COMMAND voland-cli run ${VOLAND_DEMO_NRO}
                 --expect-output "fib(90) = 2880067194370816120")
# Golden image: the core's test card must hash identically on every host.
add_test(NAME cli_golden_test_card
         COMMAND voland-cli run ${VOLAND_DEMO_NRO} --test-card --expect-frame-hash 777dcb7dfea1bd25)
add_test(NAME cli_verify_dump_nca
         COMMAND voland-cli verify-dump ${CMAKE_BINARY_DIR}/fixtures/synthetic_program.nca)
set_tests_properties(cli_verify_dump_nca PROPERTIES DEPENDS program_nca_fixture)
# Not a dump at all: verify-dump must fail (exit 66).
add_test(NAME cli_verify_dump_rejects COMMAND voland-cli verify-dump ${PROJECT_SOURCE_DIR}/CMakeLists.txt)
set_tests_properties(cli_verify_dump_rejects PROPERTIES WILL_FAIL TRUE)
