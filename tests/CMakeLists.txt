add_executable(neuralkv_unit_tests
  unit/smoke_test.cpp
  unit/status_test.cpp
  unit/storage_test.cpp
  unit/protocol_test.cpp
  unit/wal_test.cpp
  unit/cluster_config_test.cpp
  unit/raft_rpc_codec_test.cpp
)

target_link_libraries(neuralkv_unit_tests PRIVATE
  neuralkv_common
  neuralkv_storage
  neuralkv_protocol
  neuralkv_persistence
  neuralkv_cluster
  neuralkv_raft
  GTest::gtest
  GTest::gtest_main
)

neuralkv_set_warnings(neuralkv_unit_tests)

include(GoogleTest)
gtest_discover_tests(neuralkv_unit_tests)

add_executable(neuralkv_integration_tests
  integration/tcp_server_test.cpp
  integration/concurrent_server_test.cpp
  integration/crash_recovery_test.cpp
  integration/cluster_transport_test.cpp
  integration/raft_cluster_test.cpp
  integration/raft_stale_leader_test.cpp
  integration/raft_linearizable_read_test.cpp
  integration/raft_catchup_test.cpp
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
  NKV_CLIENT_PATH="$<TARGET_FILE:nkv-client>"
)

add_dependencies(neuralkv_integration_tests nkv-server nkv-client)

neuralkv_set_warnings(neuralkv_integration_tests)

gtest_discover_tests(neuralkv_integration_tests)

add_executable(neuralkv_raft_tests
  raft/raft_figure8_test.cpp
  raft/raft_fault_injection_test.cpp
)

target_link_libraries(neuralkv_raft_tests PRIVATE
  neuralkv_server
  GTest::gtest
  GTest::gtest_main
)

neuralkv_set_warnings(neuralkv_raft_tests)

gtest_discover_tests(neuralkv_raft_tests)
