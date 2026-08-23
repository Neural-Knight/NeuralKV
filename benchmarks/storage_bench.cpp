// B0 storage-ceiling benchmark (see docs/benchmark-methodology.md): measures
// ShardedKV throughput with no network or disk I/O involved, establishing
// the upper bound later stages are compared against.

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "storage/sharded_kv.h"

namespace {

constexpr std::size_t kNumKeys = 1'000'000;
constexpr std::size_t kKeySize = 16;
constexpr std::size_t kValueSize = 256;

// Zero-padded, fixed-width key so every key is exactly kKeySize bytes.
std::string MakeKey(std::size_t index) {
  std::string key(kKeySize, '0');
  std::string digits = std::to_string(index);
  std::copy(digits.rbegin(), digits.rend(), key.rbegin());
  return key;
}

std::vector<std::string> MakeKeys(std::size_t count) {
  std::vector<std::string> keys;
  keys.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    keys.push_back(MakeKey(i));
  }
  return keys;
}

std::string MakeValue() { return std::string(kValueSize, 'v'); }

}  // namespace

static void BM_Set_Sequential(benchmark::State& state) {
  const std::vector<std::string> keys = MakeKeys(kNumKeys);
  const std::string value = MakeValue();
  neuralkv::ShardedKV kv;

  for (auto _ : state) {
    for (const std::string& key : keys) {
      kv.Set(key, value);
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                           static_cast<int64_t>(kNumKeys));
}
BENCHMARK(BM_Set_Sequential);

static void BM_Get_Sequential(benchmark::State& state) {
  const std::vector<std::string> keys = MakeKeys(kNumKeys);
  const std::string value = MakeValue();
  neuralkv::ShardedKV kv;
  for (const std::string& key : keys) {
    kv.Set(key, value);
  }

  for (auto _ : state) {
    for (const std::string& key : keys) {
      benchmark::DoNotOptimize(kv.Get(key));
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                           static_cast<int64_t>(kNumKeys));
}
BENCHMARK(BM_Get_Sequential);

// 80% GET / 20% SET on a pre-populated store, matching the default workload
// mix in docs/benchmark-methodology.md.
static void BM_Set_Get_Mixed(benchmark::State& state) {
  const std::vector<std::string> keys = MakeKeys(kNumKeys);
  const std::string value = MakeValue();
  neuralkv::ShardedKV kv;
  for (const std::string& key : keys) {
    kv.Set(key, value);
  }

  for (auto _ : state) {
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (i % 5 == 0) {
        kv.Set(keys[i], value);
      } else {
        benchmark::DoNotOptimize(kv.Get(keys[i]));
      }
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                           static_cast<int64_t>(kNumKeys));
}
BENCHMARK(BM_Set_Get_Mixed);

// Measures sharded-lock scalability: each thread owns a disjoint key prefix
// so contention comes only from shard collisions, not from serialized access
// to the same keys.
static void BM_Concurrent_Mixed(benchmark::State& state) {
  static neuralkv::ShardedKV kv;
  constexpr std::size_t kKeysPerThread = 1000;

  const std::string prefix = "ct" + std::to_string(state.thread_index()) + ":";
  for (std::size_t i = 0; i < kKeysPerThread; ++i) {
    kv.Set(prefix + std::to_string(i), "value");
  }

  std::size_t i = 0;
  for (auto _ : state) {
    const std::string key = prefix + std::to_string(i % kKeysPerThread);
    if (i % 5 == 0) {
      kv.Set(key, "value");
    } else {
      benchmark::DoNotOptimize(kv.Get(key));
    }
    ++i;
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_Concurrent_Mixed)->Threads(1)->Threads(4)->Threads(8)->Threads(16);

BENCHMARK_MAIN();
