#include "raft/log.h"

#include <cerrno>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace neuralkv::raft {

Log::Log(persistence::WalWriter& wal_writer, std::string data_dir)
    : wal_writer_(wal_writer), data_dir_(std::move(data_dir)) {}

Status Log::Load() {
  entries_.clear();

  const std::string wal_path = data_dir_ + "/wal/wal.log";
  const int fd = ::open(wal_path.c_str(), O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) return Status::Ok();
    return Status::Error(ErrorCode::kIOError, std::string("open ") + wal_path + ": " + std::strerror(errno));
  }

  Status result = Status::Ok();
  while (true) {
    persistence::WalRecord record;
    bool has_record = false;
    const Status read_status = persistence::ReadNextWalRecord(fd, &record, &has_record);
    if (!read_status.ok()) {
      result = read_status;
      break;
    }
    if (!has_record) break;

    entries_.push_back(LogEntry{.term = record.term,
                                 .index = record.index,
                                 .op = record.op,
                                 .key = record.key,
                                 .value = record.value});
  }

  ::close(fd);
  return result;
}

Status Log::Append(LogEntry& entry) {
  persistence::WalRecord record;
  record.term = entry.term;
  record.op = entry.op;
  record.key = entry.key;
  record.value = entry.value;

  Status status = wal_writer_.Append(record);
  if (!status.ok()) return status;
  status = wal_writer_.Sync();
  if (!status.ok()) return status;

  entry.index = wal_writer_.last_index();
  entries_.push_back(entry);
  return Status::Ok();
}

Status Log::TruncateFrom(uint64_t from_index) {
  if (from_index == 0) {
    return Status::Error(ErrorCode::kInvalidArgument, "TruncateFrom(0) would drop the entire log");
  }

  while (!entries_.empty() && entries_.back().index >= from_index) {
    entries_.pop_back();
  }

  std::vector<persistence::WalRecord> kept;
  kept.reserve(entries_.size());
  for (const LogEntry& entry : entries_) {
    kept.push_back(persistence::WalRecord{
        .term = entry.term, .index = entry.index, .op = entry.op, .key = entry.key, .value = entry.value});
  }
  return wal_writer_.RewriteAll(kept);
}

const LogEntry* Log::Get(uint64_t index) const {
  if (index == 0 || index > entries_.size()) return nullptr;
  const LogEntry& entry = entries_[index - 1];
  if (entry.index != index) return nullptr;  // defensive: should never happen if entries stay contiguous
  return &entry;
}

std::vector<LogEntry> Log::EntriesFrom(uint64_t index) const {
  std::vector<LogEntry> result;
  if (index == 0 || index > entries_.size()) return result;
  result.assign(entries_.begin() + static_cast<std::ptrdiff_t>(index - 1), entries_.end());
  return result;
}

uint64_t Log::LastIndex() const { return entries_.empty() ? 0 : entries_.back().index; }

uint64_t Log::LastTerm() const { return entries_.empty() ? 0 : entries_.back().term; }

}  // namespace neuralkv::raft
