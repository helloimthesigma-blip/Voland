# voland-cli: the headless native runner and CI harness (DESIGN.md §17).
add_executable(voland-cli voland_cli.c)
target_link_libraries(voland-cli PRIVATE switch_core)
