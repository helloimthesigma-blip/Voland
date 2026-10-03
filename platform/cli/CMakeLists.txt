# voland-cli: the headless native runner and CI harness (DESIGN.md §17).
add_executable(voland-cli voland_cli.c video_vt.c)
target_link_libraries(voland-cli PRIVATE switch_core)
# Native video decode (DESIGN §13): VideoToolbox on macOS; elsewhere the
# requests are counted and cutscenes stay black.
target_compile_definitions(voland-cli PRIVATE VOLAND_CLI_VIDEO=1)
if(APPLE)
  target_link_libraries(voland-cli PRIVATE "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo"
                                           "-framework CoreFoundation")
endif()
