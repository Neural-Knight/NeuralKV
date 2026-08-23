add_library(neuralkv_common
  common/status.cpp
)

target_include_directories(neuralkv_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(neuralkv_common PUBLIC cxx_std_20)

neuralkv_set_warnings(neuralkv_common)
