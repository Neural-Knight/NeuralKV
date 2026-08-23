add_executable(noop_bench noop_bench.cpp)

target_link_libraries(noop_bench PRIVATE
  neuralkv_common
  benchmark::benchmark
)

neuralkv_set_warnings(noop_bench)
