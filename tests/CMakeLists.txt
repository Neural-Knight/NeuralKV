add_executable(neuralkv_unit_tests
  unit/smoke_test.cpp
  unit/status_test.cpp
  unit/storage_test.cpp
  unit/protocol_test.cpp
)

target_link_libraries(neuralkv_unit_tests PRIVATE
  neuralkv_common
  neuralkv_storage
  neuralkv_protocol
  GTest::gtest
  GTest::gtest_main
)

neuralkv_set_warnings(neuralkv_unit_tests)

include(GoogleTest)
gtest_discover_tests(neuralkv_unit_tests)

add_executable(neuralkv_integration_tests
  integration/tcp_server_test.cpp
  integration/concurrent_server_test.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(neuralkv_integration_tests PRIVATE integration/epoll_server_test.cpp)
endif()

target_link_libraries(neuralkv_integration_tests PRIVATE
  neuralkv_common
  neuralkv_protocol
  neuralkv_net
  GTest::gtest
  GTest::gtest_main
)

target_compile_definitions(neuralkv_integration_tests PRIVATE
  NKV_SERVER_PATH="$<TARGET_FILE:nkv-server>"
)

add_dependencies(neuralkv_integration_tests nkv-server)

neuralkv_set_warnings(neuralkv_integration_tests)

gtest_discover_tests(neuralkv_integration_tests)
