add_executable(noop_bench noop_bench.cpp)

target_link_libraries(noop_bench PRIVATE
  neuralkv_common
  benchmark::benchmark
)

neuralkv_set_warnings(noop_bench)

add_executable(storage_bench storage_bench.cpp)

target_link_libraries(storage_bench PRIVATE
  neuralkv_storage
  benchmark::benchmark
)

neuralkv_set_warnings(storage_bench)
