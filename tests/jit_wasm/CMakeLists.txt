# The JIT's differential test built with Emscripten and run under Node
# (docs/JIT.md): the same tests/jit_diff_test.c, but here blocks really are
# compiled to wasm and executed. Node >= 24 runs memory64 + threads.
#
# The core's link flags (top-level CMakeLists.txt) target the browser
# worker; the overrides below make a plain Node program out of the test.
# Later -s settings win, and target options come after directory ones.

find_program(VOLAND_NODE node REQUIRED)

add_executable(jit_diff_test_wasm ../jit_diff_test.c)
target_link_libraries(jit_diff_test_wasm PRIVATE switch_core)
target_include_directories(jit_diff_test_wasm PRIVATE ..)
target_link_options(jit_diff_test_wasm PRIVATE
  -sENVIRONMENT=node,worker
  -sIMPORTED_MEMORY=0
  -sMODULARIZE=0
  -sEXPORT_ES6=0
  -sEXIT_RUNTIME=1
  -sPTHREAD_POOL_SIZE=0
  -sNODERAWFS=1
  "-sEXPORTED_FUNCTIONS=['_main']"
)
set_target_properties(jit_diff_test_wasm PROPERTIES OUTPUT_NAME "jit_diff_test" SUFFIX ".js")

# Micro-benchmark (not a test): node build/web-jit/tests/jit_wasm/jit_bench.js [N]
add_executable(jit_bench_wasm EXCLUDE_FROM_ALL ../jit_bench.c)
target_link_libraries(jit_bench_wasm PRIVATE switch_core)
target_include_directories(jit_bench_wasm PRIVATE ..)
target_link_options(jit_bench_wasm PRIVATE
  -sENVIRONMENT=node,worker -sIMPORTED_MEMORY=0 -sMODULARIZE=0 -sEXPORT_ES6=0 -sEXIT_RUNTIME=1
  -sPTHREAD_POOL_SIZE=0 -sNODERAWFS=1 "-sEXPORTED_FUNCTIONS=['_main']")
set_target_properties(jit_bench_wasm PROPERTIES OUTPUT_NAME "jit_bench" SUFFIX ".js")

add_test(NAME jit_diff_test_node
  COMMAND ${VOLAND_NODE} $<TARGET_FILE:jit_diff_test_wasm> 20000)

# The A32 front end against the A32 interpreter, compiled for real.
add_executable(a32_jit_diff_test_wasm ../a32_jit_diff_test.c)
target_link_libraries(a32_jit_diff_test_wasm PRIVATE switch_core)
target_include_directories(a32_jit_diff_test_wasm PRIVATE ..)
target_link_options(a32_jit_diff_test_wasm PRIVATE
  -sENVIRONMENT=node,worker -sIMPORTED_MEMORY=0 -sMODULARIZE=0 -sEXPORT_ES6=0 -sEXIT_RUNTIME=1
  -sPTHREAD_POOL_SIZE=0 -sNODERAWFS=1 "-sEXPORTED_FUNCTIONS=['_main']")
set_target_properties(a32_jit_diff_test_wasm PROPERTIES OUTPUT_NAME "a32_jit_diff_test" SUFFIX ".js")
add_test(NAME a32_jit_diff_test_node
  COMMAND ${VOLAND_NODE} $<TARGET_FILE:a32_jit_diff_test_wasm> 20000)

# voland-cli as a Node program: the real-workload harness for the JIT
# (docs/JIT.md "Measuring"). Same core and flags as the browser build, so
# `--backend interpreter` vs `--backend jit` compares what the browser
# would run, in the same engine (V8). Not a test; build it explicitly:
#   cmake --build --preset web-jit --target voland-cli-node
add_executable(voland-cli-node EXCLUDE_FROM_ALL ${PROJECT_SOURCE_DIR}/platform/cli/voland_cli.c)
target_link_libraries(voland-cli-node PRIVATE switch_core)
target_link_options(voland-cli-node PRIVATE
  --profiling-funcs          # names in node --cpu-prof; no codegen change
  -sENVIRONMENT=node,worker
  -sIMPORTED_MEMORY=0
  -sMODULARIZE=0
  -sEXPORT_ES6=0
  -sEXIT_RUNTIME=1
  -sNODERAWFS=1
  "-sEXPORTED_FUNCTIONS=['_main']"
)
set_target_properties(voland-cli-node PROPERTIES OUTPUT_NAME "voland-cli" SUFFIX ".js")
