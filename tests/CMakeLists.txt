add_executable(neuralkv_unit_tests
  unit/smoke_test.cpp
  unit/status_test.cpp
)

target_link_libraries(neuralkv_unit_tests PRIVATE
  neuralkv_common
  GTest::gtest
  GTest::gtest_main
)

neuralkv_set_warnings(neuralkv_unit_tests)

include(GoogleTest)
gtest_discover_tests(neuralkv_unit_tests)
