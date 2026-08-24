#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/status.h"
#include "persistence/wal_writer.h"
#include "raft/types.h"

namespace neuralkv::raft {

// A node's Raft log, backed directly by its WAL: every entry appended here
// is also the durable WAL record for that index. Not internally
// synchronized — callers (RaftNode) serialize access with their own lock.
class Log {
 public:
  // wal_writer is borrowed from the owning DurableStorage; data_dir names the
  // same directory, used for the read-only startup scan that populates entries_
  // (WalWriter's own scan only tracks the highest index, not full contents).
  Log(persistence::WalWriter& wal_writer, std::string data_dir);

  // Reads data_dir/wal/wal.log into the in-memory entries vector. Call once
  // at startup before any Append/TruncateFrom.
  Status Load();

  // Assigns entry.index (one past the current last index), writes it
  // through to the WAL, fsyncs, and appends it in memory.
  Status Append(LogEntry& entry);

  // Drops every entry with index >= from_index and rewrites the WAL to
  // match — used when a follower's log conflicts with its leader's.
  Status TruncateFrom(uint64_t from_index);

  const LogEntry* Get(uint64_t index) const;
  std::vector<LogEntry> EntriesFrom(uint64_t index) const;

  uint64_t LastIndex() const;
  uint64_t LastTerm() const;

 private:
  persistence::WalWriter& wal_writer_;
  std::string data_dir_;
  std::vector<LogEntry> entries_;  // entries_[i] holds index i+1
};

}  // namespace neuralkv::raft
