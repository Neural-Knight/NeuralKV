#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/status.h"
#include "persistence/wal_record.h"

namespace neuralkv::persistence {

// Append-only writer for data_dir/wal/wal.log. Move-only: owns the file
// descriptor directly and closes it on destruction.
class WalWriter {
 public:
  explicit WalWriter(std::string data_dir);
  ~WalWriter();

  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;
  WalWriter(WalWriter&& other) noexcept;
  WalWriter& operator=(WalWriter&& other) noexcept;

  // Assigns record.index = ++last_index() and appends it to the log.
  // record.term is written as given (callers outside Raft leave it 0).
  // Does not fsync; call Sync() to make the append durable.
  Status Append(WalRecord record);

  // fsyncs the log file.
  Status Sync();

  // Replaces the entire log with records, in order, fsync'd. Used for Raft
  // log conflict resolution (truncate-and-reappend), where an arbitrary
  // suffix of previously-written entries needs to disappear — something an
  // append-only file can't do incrementally. last_index() becomes
  // records.back().index, or 0 if records is empty.
  Status RewriteAll(const std::vector<WalRecord>& records);

  uint64_t last_index() const { return last_index_; }
  std::string wal_path() const { return wal_path_; }

 private:
  Status OpenOrCreate();

  std::string data_dir_;
  int fd_ = -1;
  std::string wal_path_;
  uint64_t last_index_ = 0;
  Status open_status_ = Status::Ok();
};

}  // namespace neuralkv::persistence
