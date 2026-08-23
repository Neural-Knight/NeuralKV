add_executable(nkv-server main.cpp)

target_link_libraries(nkv-server PRIVATE neuralkv_server)

neuralkv_set_warnings(nkv-server)
