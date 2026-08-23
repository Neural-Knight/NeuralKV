#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "common/result.h"
#include "common/status.h"
#include "persistence/wal_writer.h"
#include "storage/sharded_kv.h"

namespace neuralkv::persistence {

// Durable KV: every SET/DELETE is appended to the WAL and fsync'd before
// it's applied to the in-memory store, so an acknowledged write survives a
// crash. GET reads straight from memory — recovery already replayed
// everything the WAL had fsync'd, so the log path has nothing to add on
// the read side.
class DurableStorage {
 public:
  explicit DurableStorage(std::string data_dir);

  // Constructs storage rooted at data_dir and replays its WAL. Returns the
  // recovery error instead of a usable instance if the log is corrupt.
  static Result<DurableStorage> Open(std::string data_dir);

  Status Set(std::string_view key, std::string_view value);
  Result<std::string> Get(std::string_view key) const;
  Status Delete(std::string_view key);

  // Applies an already-durable record (its WAL entry was written by
  // Raft's log, not here) to the in-memory store and advances
  // last_applied_index. Used only by the Raft apply path — single-node
  // callers go through Set/Delete, which write their own WAL entries.
  void ApplyCommitted(const WalRecord& record);

  uint64_t last_applied_index() const { return last_applied_index_; }

  const std::string& data_dir() const { return data_dir_; }
  WalWriter& wal_writer() { return wal_; }

  ShardedKV& kv() { return *kv_; }

 private:
  std::string data_dir_;
  std::unique_ptr<ShardedKV> kv_ = std::make_unique<ShardedKV>();
  WalWriter wal_;
  uint64_t last_applied_index_ = 0;
  Status recovery_status_ = Status::Ok();
  // Serializes the append-fsync-apply sequence across concurrent writers
  // (thread-pool workers). Held via unique_ptr so DurableStorage stays
  // movable — needed to return it from Open().
  std::unique_ptr<std::mutex> write_mutex_ = std::make_unique<std::mutex>();
};

}  // namespace neuralkv::persistence
