#include "persistence/durable_storage.h"

#include <utility>

#include "persistence/recovery.h"
#include "persistence/wal_record.h"

namespace neuralkv::persistence {

DurableStorage::DurableStorage(std::string data_dir) : data_dir_(data_dir), wal_(data_dir) {
  recovery_status_ = RecoverFromWal(data_dir_, *kv_, &last_applied_index_);
}

Result<DurableStorage> DurableStorage::Open(std::string data_dir) {
  DurableStorage storage(std::move(data_dir));
  if (!storage.recovery_status_.ok()) {
    return storage.recovery_status_;
  }
  return storage;
}

Status DurableStorage::Set(std::string_view key, std::string_view value) {
  if (key.empty()) {
    return Status::Error(ErrorCode::kInvalidArgument, "key must not be empty");
  }
  if (value.empty()) {
    return Status::Error(ErrorCode::kInvalidArgument, "SET requires a non-empty value");
  }

  std::lock_guard<std::mutex> lock(*write_mutex_);

  WalRecord record;
  record.op = WalOp::kSet;
  record.key = std::string(key);
  record.value = std::string(value);

  Status status = wal_.Append(std::move(record));
  if (!status.ok()) return status;
  status = wal_.Sync();
  if (!status.ok()) return status;
  last_applied_index_ = wal_.last_index();

  return kv_->Set(key, value);
}

Result<std::string> DurableStorage::Get(std::string_view key) const { return kv_->Get(key); }

Status DurableStorage::Delete(std::string_view key) {
  if (key.empty()) {
    return Status::Error(ErrorCode::kInvalidArgument, "key must not be empty");
  }

  std::lock_guard<std::mutex> lock(*write_mutex_);

  WalRecord record;
  record.op = WalOp::kDelete;
  record.key = std::string(key);

  Status status = wal_.Append(std::move(record));
  if (!status.ok()) return status;
  status = wal_.Sync();
  if (!status.ok()) return status;
  last_applied_index_ = wal_.last_index();

  return kv_->Delete(key);
}

void DurableStorage::ApplyCommitted(const WalRecord& record) {
  switch (record.op) {
    case WalOp::kSet:
      kv_->Set(record.key, record.value);
      break;
    case WalOp::kDelete:
      kv_->Delete(record.key);
      break;
  }
  last_applied_index_ = record.index;
}

}  // namespace neuralkv::persistence
