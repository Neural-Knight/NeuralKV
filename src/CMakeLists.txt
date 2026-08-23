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
