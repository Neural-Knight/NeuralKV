add_executable(nkv-client main.cpp)

target_link_libraries(nkv-client PRIVATE
  neuralkv_protocol
  neuralkv_net
)

neuralkv_set_warnings(nkv-client)
