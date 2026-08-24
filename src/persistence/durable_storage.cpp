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

  WalRecord record;
  record.op = WalOp::kSet;
  record.key = std::string(key);
  record.value = std::string(value);

  Result<uint64_t> appended = wal_.Append(std::move(record));
  if (!appended.ok()) return appended.status();
  const uint64_t index = appended.value();

  Status status = wal_.Sync(index);
  if (!status.ok()) return status;

  // Group commit lets Append/Sync complete out of order across threads;
  // apply order must still match WAL order for single-node semantics to
  // hold, so wait for every earlier record to be applied first.
  Status set_status = Status::Ok();
  {
    std::unique_lock<std::mutex> lock(*apply_mutex_);
    apply_cv_->wait(lock, [&] { return last_applied_index_ == index - 1; });
    set_status = kv_->Set(key, value);
    last_applied_index_ = index;
  }
  apply_cv_->notify_all();

  return set_status;
}

Result<std::string> DurableStorage::Get(std::string_view key) const { return kv_->Get(key); }

Status DurableStorage::Delete(std::string_view key) {
  if (key.empty()) {
    return Status::Error(ErrorCode::kInvalidArgument, "key must not be empty");
  }

  WalRecord record;
  record.op = WalOp::kDelete;
  record.key = std::string(key);

  Result<uint64_t> appended = wal_.Append(std::move(record));
  if (!appended.ok()) return appended.status();
  const uint64_t index = appended.value();

  Status status = wal_.Sync(index);
  if (!status.ok()) return status;

  Status delete_status = Status::Ok();
  {
    std::unique_lock<std::mutex> lock(*apply_mutex_);
    apply_cv_->wait(lock, [&] { return last_applied_index_ == index - 1; });
    delete_status = kv_->Delete(key);
    last_applied_index_ = index;
  }
  apply_cv_->notify_all();

  return delete_status;
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
