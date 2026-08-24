#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/result.h"
#include "common/status.h"
#include "persistence/wal_record.h"

namespace neuralkv::persistence {

// Append-only writer for data_dir/wal/wal.log. Thread-safe: Append just writes
// bytes and assigns an index; Sync batches concurrent callers into as few real
// fsyncs as possible (group commit) while still fsync'ing before returning to each.
class WalWriter {
 public:
  explicit WalWriter(std::string data_dir);
  ~WalWriter();

  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;
  WalWriter(WalWriter&& other) noexcept;
  WalWriter& operator=(WalWriter&& other) noexcept;

  // Assigns record.index = ++last_index() and appends it, returning the
  // assigned index. Does not fsync — call Sync(assigned_index) to make it
  // durable. Safe to call concurrently from multiple threads.
  Result<uint64_t> Append(WalRecord record);

  // Blocks until every record up to at_least_index is fsync'd. Concurrent
  // callers are batched into one real fsync where possible (group commit),
  // bounded by kGroupCommitMaxRecords or kGroupCommitMaxDelay, whichever comes first.
  Status Sync(uint64_t at_least_index);

  // Replaces the entire log with records, in order, fsync'd immediately —
  // bypasses group-commit batching since this is a rare, exclusive operation.
  // Used for Raft log conflict resolution; last_index() becomes records.back().index, or 0 if empty.
  Status RewriteAll(const std::vector<WalRecord>& records);

  uint64_t last_index() const;
  std::string wal_path() const { return wal_path_; }

 private:
  Status OpenOrCreate();
  Status FsyncNow();  // caller must hold mutex_

  std::string data_dir_;
  int fd_ = -1;
  std::string wal_path_;
  uint64_t last_index_ = 0;
  Status open_status_ = Status::Ok();

  // Heap-allocated so WalWriter stays move-constructible (DurableStorage
  // returns itself by value from Open(), which requires moving this).
  std::unique_ptr<std::mutex> mutex_ = std::make_unique<std::mutex>();
  std::unique_ptr<std::condition_variable> cv_ = std::make_unique<std::condition_variable>();
  uint64_t synced_index_ = 0;
  bool flush_in_progress_ = false;

  // Real fsync() calls made so far — how group commit's coalescing is
  // actually measured. Not part of the public API: only
  // tests/unit/group_commit_test.cpp defines this function's body.
  uint64_t fsync_count_ = 0;
  friend uint64_t TestOnlyFsyncCount(const WalWriter& writer);
};

// Group commit tuning: batch up to this many pending (appended, not yet
// fsync'd) records into one fsync, or wait at most this long for more
// to arrive before flushing whatever's pending — whichever comes first.
inline constexpr int kGroupCommitMaxRecords = 16;
inline constexpr std::chrono::milliseconds kGroupCommitMaxDelay{1};

// The batch leader polls in short ticks rather than sleeping to the full delay
// cap, so it can flush as soon as a tick passes with no new arrivals instead
// of always waiting out the cap when there's nothing to batch with.
inline constexpr std::chrono::microseconds kGroupCommitQuietTick{50};

}  // namespace neuralkv::persistence
