# Parallel guest threads (docs/PARALLEL.md) under Emscripten pthreads, run
# by Node: the same tests/parallel_test.c, with core threads that are real
# Emscripten pthreads and a driver that is the main runtime thread, as the
# CPU worker is in the browser. Built only on request (EXCLUDE_FROM_ALL):
#   cmake --build --preset web --target parallel_test_wasm parallel-cli-node
#   node build/web/tests/parallel_wasm/parallel_test.js
#
# The core's link flags (top-level CMakeLists.txt) target the browser
# worker; the overrides below make a plain Node program. Later -s settings
# win, and target options come after directory ones. The pthread pool
# stays prepopulated, as in the browser.

find_program(VOLAND_NODE node REQUIRED)

set(PARALLEL_NODE_LINK_OPTIONS
  -sENVIRONMENT=node,worker
  -sIMPORTED_MEMORY=0
  -sMODULARIZE=0
  -sEXPORT_ES6=0
  -sEXIT_RUNTIME=1
  -sNODERAWFS=1
  "-sEXPORTED_FUNCTIONS=['_main']"
)

add_executable(parallel_test_wasm EXCLUDE_FROM_ALL
  ../parallel_test.c ../guest_fixture.c ../loader_fixtures.c)
target_link_libraries(parallel_test_wasm PRIVATE switch_core)
target_include_directories(parallel_test_wasm PRIVATE ..)
target_link_options(parallel_test_wasm PRIVATE --profiling-funcs ${PARALLEL_NODE_LINK_OPTIONS})
set_target_properties(parallel_test_wasm PROPERTIES OUTPUT_NAME "parallel_test" SUFFIX ".js")
add_test(NAME parallel_test_node COMMAND ${VOLAND_NODE} $<TARGET_FILE:parallel_test_wasm>)

# voland-cli as a Node program, for measuring --host-cores in V8.
add_executable(parallel-cli-node EXCLUDE_FROM_ALL ${PROJECT_SOURCE_DIR}/platform/cli/voland_cli.c)
target_link_libraries(parallel-cli-node PRIVATE switch_core)
target_link_options(parallel-cli-node PRIVATE --profiling-funcs ${PARALLEL_NODE_LINK_OPTIONS})
set_target_properties(parallel-cli-node PROPERTIES OUTPUT_NAME "voland-cli" SUFFIX ".js")
