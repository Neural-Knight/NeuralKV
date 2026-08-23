#include "persistence/wal_writer.h"

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
      open_status_(std::move(other.open_status_)) {
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

  // Scan the existing log once to learn the highest index already
  // written, so new appends continue the sequence. Recovery does the
  // authoritative replay into the KV separately; this pass only needs
  // the index.
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
  return Status::Ok();
}

Status WalWriter::Append(WalRecord record) {
  if (!open_status_.ok()) return open_status_;

  record.term = 0;
  record.index = last_index_ + 1;
  std::vector<uint8_t> encoded;
  EncodeWalRecord(record, encoded);

  std::size_t written = 0;
  while (written < encoded.size()) {
    const ssize_t n = ::write(fd_, encoded.data() + written, encoded.size() - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::Error(ErrorCode::kIOError, std::string("write wal: ") + std::strerror(errno));
    }
    written += static_cast<std::size_t>(n);
  }

  last_index_ = record.index;
  return Status::Ok();
}

Status WalWriter::Sync() {
  if (!open_status_.ok()) return open_status_;
  if (::fsync(fd_) != 0) {
    return Status::Error(ErrorCode::kIOError, std::string("fsync wal: ") + std::strerror(errno));
  }
  return Status::Ok();
}

}  // namespace neuralkv::persistence
