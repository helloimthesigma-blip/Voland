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

add_test(NAME jit_diff_test_node
  COMMAND ${VOLAND_NODE} $<TARGET_FILE:jit_diff_test_wasm> 20000)
