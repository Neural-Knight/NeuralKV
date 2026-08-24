#include "persistence/wal_writer.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace neuralkv::persistence {

namespace {

// mkdir -p: creates every missing component of path.
Status MakeDirectoryRecursive(const std::string& path) {
  std::string partial;
  std::size_t pos = 0;
  if (!path.empty() && path.front() == '/') {
    partial = "/";
    pos = 1;
  }
  while (pos <= path.size()) {
    std::size_t next = path.find('/', pos);
    if (next == std::string::npos) next = path.size();
    partial += path.substr(pos, next - pos);
    if (!partial.empty() && ::mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST) {
      return Status::Error(ErrorCode::kIOError,
                            std::string("mkdir ") + partial + ": " + std::strerror(errno));
    }
    partial += "/";
    pos = next + 1;
  }
  return Status::Ok();
}

}  // namespace

WalWriter::WalWriter(std::string data_dir) : data_dir_(std::move(data_dir)) {
  open_status_ = OpenOrCreate();
}

WalWriter::~WalWriter() {
  if (fd_ >= 0) ::close(fd_);
}

WalWriter::WalWriter(WalWriter&& other) noexcept
    : data_dir_(std::move(other.data_dir_)),
      fd_(other.fd_),
      wal_path_(std::move(other.wal_path_)),
      last_index_(other.last_index_),
      open_status_(std::move(other.open_status_)),
      mutex_(std::move(other.mutex_)),
      cv_(std::move(other.cv_)),
      synced_index_(other.synced_index_),
      flush_in_progress_(other.flush_in_progress_),
      fsync_count_(other.fsync_count_) {
  other.fd_ = -1;
}

WalWriter& WalWriter::operator=(WalWriter&& other) noexcept {
  if (this == &other) return *this;
  if (fd_ >= 0) ::close(fd_);
  data_dir_ = std::move(other.data_dir_);
  fd_ = other.fd_;
  wal_path_ = std::move(other.wal_path_);
  last_index_ = other.last_index_;
  open_status_ = std::move(other.open_status_);
  mutex_ = std::move(other.mutex_);
  cv_ = std::move(other.cv_);
  synced_index_ = other.synced_index_;
  flush_in_progress_ = other.flush_in_progress_;
  fsync_count_ = other.fsync_count_;
  other.fd_ = -1;
  return *this;
}

Status WalWriter::OpenOrCreate() {
  const std::string wal_dir = data_dir_ + "/wal";
  Status status = MakeDirectoryRecursive(wal_dir);
  if (!status.ok()) return status;

  wal_path_ = wal_dir + "/wal.log";
  fd_ = ::open(wal_path_.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
  if (fd_ < 0) {
    return Status::Error(ErrorCode::kIOError,
                          std::string("open ") + wal_path_ + ": " + std::strerror(errno));
  }

  // Scans the existing log once to learn the highest index already written,
  // so new appends continue the sequence. Recovery does the authoritative
  // replay into the KV separately — this only needs the index.
  if (::lseek(fd_, 0, SEEK_SET) < 0) {
    return Status::Error(ErrorCode::kIOError,
                          std::string("lseek ") + wal_path_ + ": " + std::strerror(errno));
  }
  while (true) {
    WalRecord record;
    bool has_record = false;
    const Status read_status = ReadNextWalRecord(fd_, &record, &has_record);
    if (!read_status.ok()) return read_status;
    if (!has_record) break;
    last_index_ = record.index;
  }
  synced_index_ = last_index_;  // everything on disk at open time is already durable
  return Status::Ok();
}

namespace {

Status WriteAll(int fd, const std::vector<uint8_t>& bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t n = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::Error(ErrorCode::kIOError, std::string("write wal: ") + std::strerror(errno));
    }
    written += static_cast<std::size_t>(n);
  }
  return Status::Ok();
}

}  // namespace

Result<uint64_t> WalWriter::Append(WalRecord record) {
  std::lock_guard<std::mutex> lock(*mutex_);
  if (!open_status_.ok()) return open_status_;

  record.index = last_index_ + 1;
  std::vector<uint8_t> encoded;
  EncodeWalRecord(record, encoded);

  Status status = WriteAll(fd_, encoded);
  if (!status.ok()) return status;

  last_index_ = record.index;
  cv_->notify_all();  // wake anyone group-committing, so they can re-check their batch cap
  return last_index_;
}

Status WalWriter::FsyncNow() {
  if (::fsync(fd_) != 0) {
    return Status::Error(ErrorCode::kIOError, std::string("fsync wal: ") + std::strerror(errno));
  }
  ++fsync_count_;
  return Status::Ok();
}

Status WalWriter::Sync(uint64_t at_least_index) {
  std::unique_lock<std::mutex> lock(*mutex_);
  if (!open_status_.ok()) return open_status_;

  if (synced_index_ >= at_least_index) return Status::Ok();  // a prior flush already covered this

  if (flush_in_progress_) {
    // Someone else is already the batch leader; wait for their flush
    // (or a later one) to cover our index too.
    cv_->wait(lock, [&] { return synced_index_ >= at_least_index || !flush_in_progress_; });
    if (synced_index_ >= at_least_index) return Status::Ok();
    // The flush we were waiting on finished without covering us (can
    // only happen if this WalWriter's open_status_ turned bad
    // mid-flush); fall through and become the leader for our own index.
  }

  // We're the batch leader: wait for more appends up to the record or delay
  // cap, then fsync everything accumulated in one call. The wait polls in
  // short quiet-ticks and stops early once nothing new arrives, so an uncontended Append+Sync pays only a small bounded delay instead of the full cap.
  flush_in_progress_ = true;
  const auto deadline = std::chrono::steady_clock::now() + kGroupCommitMaxDelay;
  uint64_t observed_index = last_index_;
  while (static_cast<int>(last_index_ - synced_index_) < kGroupCommitMaxRecords) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) break;
    cv_->wait_until(lock, std::min(deadline, now + kGroupCommitQuietTick));
    if (last_index_ == observed_index) break;  // quiet: nothing new arrived this tick
    observed_index = last_index_;
  }

  const Status status = FsyncNow();
  if (status.ok()) {
    synced_index_ = last_index_;
  }
  flush_in_progress_ = false;
  cv_->notify_all();

  if (!status.ok()) return status;
  return synced_index_ >= at_least_index ? Status::Ok() : status;
}

Status WalWriter::RewriteAll(const std::vector<WalRecord>& records) {
  std::lock_guard<std::mutex> lock(*mutex_);
  if (!open_status_.ok()) return open_status_;

  if (::ftruncate(fd_, 0) != 0) {
    return Status::Error(ErrorCode::kIOError, std::string("ftruncate wal: ") + std::strerror(errno));
  }
  if (::lseek(fd_, 0, SEEK_SET) < 0) {
    return Status::Error(ErrorCode::kIOError, std::string("lseek wal: ") + std::strerror(errno));
  }

  for (const WalRecord& record : records) {
    std::vector<uint8_t> encoded;
    EncodeWalRecord(record, encoded);
    Status status = WriteAll(fd_, encoded);
    if (!status.ok()) return status;
  }

  Status status = FsyncNow();
  if (!status.ok()) return status;

  last_index_ = records.empty() ? 0 : records.back().index;
  synced_index_ = last_index_;
  cv_->notify_all();
  return Status::Ok();
}

uint64_t WalWriter::last_index() const {
  std::lock_guard<std::mutex> lock(*mutex_);
  return last_index_;
}

}  // namespace neuralkv::persistence
