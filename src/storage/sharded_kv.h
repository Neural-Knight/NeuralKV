#pragma once

#include <array>
#include <cstddef>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "common/result.h"
#include "common/status.h"

namespace neuralkv {

// Thread-safe in-memory key-value store using lock striping: keys are
// distributed across a fixed number of independently-locked shards so
// unrelated keys don't contend on the same mutex.
class ShardedKV {
 public:
  static constexpr std::size_t kShardCount = 256;

  ShardedKV() = default;

  Status Set(std::string_view key, std::string_view value);
  Result<std::string> Get(std::string_view key) const;
  Status Delete(std::string_view key);

  std::size_t Size() const;

 private:
  struct Shard {
    mutable std::shared_mutex mutex;
    std::unordered_map<std::string, std::string> data;
  };

  static std::size_t ShardFor(std::string_view key);

  std::array<Shard, kShardCount> shards_;
};

}  // namespace neuralkv
