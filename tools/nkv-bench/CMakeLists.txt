add_executable(nkv-bench main.cpp)

target_link_libraries(nkv-bench PRIVATE
  neuralkv_common
  neuralkv_protocol
  neuralkv_net
)

neuralkv_set_warnings(nkv-bench)
