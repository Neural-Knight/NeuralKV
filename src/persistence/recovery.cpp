#include "persistence/recovery.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include "persistence/wal_record.h"

namespace neuralkv::persistence {

Status RecoverFromWal(const std::string& data_dir, ShardedKV& kv, uint64_t* last_applied_index) {
  *last_applied_index = 0;

  const std::string wal_path = data_dir + "/wal/wal.log";
  const int fd = ::open(wal_path.c_str(), O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) return Status::Ok();
    return Status::Error(ErrorCode::kIOError, std::string("open ") + wal_path + ": " + std::strerror(errno));
  }

  Status result = Status::Ok();
  while (true) {
    WalRecord record;
    bool has_record = false;
    const Status read_status = ReadNextWalRecord(fd, &record, &has_record);
    if (!read_status.ok()) {
      result = read_status;
      break;
    }
    if (!has_record) break;

    switch (record.op) {
      case WalOp::kSet:
        kv.Set(record.key, record.value);
        break;
      case WalOp::kDelete:
        kv.Delete(record.key);
        break;
    }
    *last_applied_index = record.index;
  }

  ::close(fd);
  return result;
}

}  // namespace neuralkv::persistence
