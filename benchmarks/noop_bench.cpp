#include <benchmark/benchmark.h>

// Sanity check that the benchmark harness links and runs correctly;
// no production code exists yet worth measuring.
static void BM_EmptyLoop(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(state.iterations());
  }
}
BENCHMARK(BM_EmptyLoop);

BENCHMARK_MAIN();
