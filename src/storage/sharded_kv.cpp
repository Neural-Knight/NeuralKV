#include "storage/sharded_kv.h"

#include <mutex>
#include <shared_mutex>

namespace neuralkv {

std::size_t ShardedKV::ShardFor(std::string_view key) {
  return std::hash<std::string_view>{}(key) % kShardCount;
}

Status ShardedKV::Set(std::string_view key, std::string_view value) {
  if (key.empty()) {
    return Status::Error(ErrorCode::kInvalidArgument, "key must not be empty");
  }

  Shard& shard = shards_[ShardFor(key)];
  std::unique_lock lock(shard.mutex);
  shard.data.insert_or_assign(std::string(key), std::string(value));
  return Status::Ok();
}

Result<std::string> ShardedKV::Get(std::string_view key) const {
  if (key.empty()) {
    return Status::Error(ErrorCode::kInvalidArgument, "key must not be empty");
  }

  const Shard& shard = shards_[ShardFor(key)];
  std::shared_lock lock(shard.mutex);
  auto it = shard.data.find(std::string(key));
  if (it == shard.data.end()) {
    return Status::Error(ErrorCode::kNotFound, "key not found");
  }
  return it->second;
}

Status ShardedKV::Delete(std::string_view key) {
  if (key.empty()) {
    return Status::Error(ErrorCode::kInvalidArgument, "key must not be empty");
  }

  Shard& shard = shards_[ShardFor(key)];
  std::unique_lock lock(shard.mutex);
  auto it = shard.data.find(std::string(key));
  if (it == shard.data.end()) {
    return Status::Error(ErrorCode::kNotFound, "key not found");
  }
  shard.data.erase(it);
  return Status::Ok();
}

std::size_t ShardedKV::Size() const {
  std::size_t total = 0;
  for (const Shard& shard : shards_) {
    std::shared_lock lock(shard.mutex);
    total += shard.data.size();
  }
  return total;
}

}  // namespace neuralkv
