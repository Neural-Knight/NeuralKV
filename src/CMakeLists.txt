add_library(neuralkv_common
  common/status.cpp
)

target_include_directories(neuralkv_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(neuralkv_common PUBLIC cxx_std_20)

neuralkv_set_warnings(neuralkv_common)

add_library(neuralkv_storage
  storage/sharded_kv.cpp
)

target_include_directories(neuralkv_storage PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(neuralkv_storage PUBLIC neuralkv_common)

target_compile_features(neuralkv_storage PUBLIC cxx_std_20)

neuralkv_set_warnings(neuralkv_storage)

add_library(neuralkv_protocol
  protocol/codec.cpp
)

target_include_directories(neuralkv_protocol PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(neuralkv_protocol PUBLIC neuralkv_common)

target_compile_features(neuralkv_protocol PUBLIC cxx_std_20)

neuralkv_set_warnings(neuralkv_protocol)

add_library(neuralkv_net
  net/socket_utils.cpp
)

target_include_directories(neuralkv_net PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(neuralkv_net PUBLIC neuralkv_common)

target_compile_features(neuralkv_net PUBLIC cxx_std_20)

neuralkv_set_warnings(neuralkv_net)

add_library(neuralkv_server
  server/handler.cpp
  server/blocking_server.cpp
)

target_include_directories(neuralkv_server PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(neuralkv_server PUBLIC
  neuralkv_storage
  neuralkv_protocol
  neuralkv_net
)

target_compile_features(neuralkv_server PUBLIC cxx_std_20)

neuralkv_set_warnings(neuralkv_server)
