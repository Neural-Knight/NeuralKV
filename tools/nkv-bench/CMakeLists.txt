add_executable(nkv-bench main.cpp)

target_link_libraries(nkv-bench PRIVATE neuralkv_common)

neuralkv_set_warnings(nkv-bench)
