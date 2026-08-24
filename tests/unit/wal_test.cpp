#include "persistence/wal_writer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include "common/result.h"
#include "common/status.h"
#include "persistence/recovery.h"
#include "persistence/wal_record.h"
#include "storage/sharded_kv.h"

namespace neuralkv::persistence {
namespace {

// Fresh temp directory for one test's WAL; removes the files WalWriter
// creates in it (data_dir/wal/wal.log and the wal/ subdirectory) on
// destruction.
class TempDataDir {
 public:
  TempDataDir() {
    char pattern[] = "/tmp/nkv_wal_test_XXXXXX";
    const char* dir = ::mkdtemp(pattern);
    if (dir == nullptr) {
      std::perror("mkdtemp");
      std::abort();
    }
    path_ = dir;
  }

  ~TempDataDir() {
    ::unlink((path_ + "/wal/wal.log").c_str());
    ::rmdir((path_ + "/wal").c_str());
    ::rmdir(path_.c_str());
  }

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

TEST(WalTest, AppendAndSyncWritesCrcValidRecord) {
  TempDataDir dir;
  WalWriter writer(dir.path());

  WalRecord record;
  record.op = WalOp::kSet;
  record.key = "k1";
  record.value = "v1";
  Result<uint64_t> appended = writer.Append(record);
  ASSERT_TRUE(appended.ok());
  ASSERT_TRUE(writer.Sync(appended.value()).ok());

  const int fd = ::open(writer.wal_path().c_str(), O_RDONLY);
  ASSERT_GE(fd, 0);
  WalRecord read_back;
  bool has_record = false;
  ASSERT_TRUE(ReadNextWalRecord(fd, &read_back, &has_record).ok());
  ::close(fd);

  ASSERT_TRUE(has_record);
  EXPECT_EQ(read_back.op, WalOp::kSet);
  EXPECT_EQ(read_back.key, "k1");
  EXPECT_EQ(read_back.value, "v1");
  EXPECT_EQ(read_back.index, 1u);
  EXPECT_EQ(read_back.term, 0u);
}

TEST(WalTest, RecoverReplaysSetAndDeleteInOrder) {
  TempDataDir dir;
  {
    WalWriter writer(dir.path());

    WalRecord set1;
    set1.op = WalOp::kSet;
    set1.key = "k1";
    set1.value = "v1";
    ASSERT_TRUE(writer.Append(set1).ok());

    WalRecord set2;
    set2.op = WalOp::kSet;
    set2.key = "k2";
    set2.value = "v2";
    ASSERT_TRUE(writer.Append(set2).ok());

    WalRecord del1;
    del1.op = WalOp::kDelete;
    del1.key = "k1";
    Result<uint64_t> appended = writer.Append(del1);
    ASSERT_TRUE(appended.ok());
    ASSERT_TRUE(writer.Sync(appended.value()).ok());
  }

  ShardedKV kv;
  uint64_t last_applied_index = 0;
  ASSERT_TRUE(RecoverFromWal(dir.path(), kv, &last_applied_index).ok());
  EXPECT_EQ(last_applied_index, 3u);

  EXPECT_FALSE(kv.Get("k1").ok());
  Result<std::string> v2 = kv.Get("k2");
  ASSERT_TRUE(v2.ok());
  EXPECT_EQ(v2.value(), "v2");
}

TEST(WalTest, RecoverIgnoresTruncatedTailRecord) {
  TempDataDir dir;
  {
    WalWriter writer(dir.path());
    WalRecord set1;
    set1.op = WalOp::kSet;
    set1.key = "k1";
    set1.value = "v1";
    Result<uint64_t> appended = writer.Append(set1);
    ASSERT_TRUE(appended.ok());
    ASSERT_TRUE(writer.Sync(appended.value()).ok());
  }

  // Simulate a crash mid-write: a full valid record followed by a few
  // header bytes of a second record that never got the rest written.
  const int fd = ::open((dir.path() + "/wal/wal.log").c_str(), O_WRONLY | O_APPEND);
  ASSERT_GE(fd, 0);
  const uint8_t partial_header[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  ASSERT_EQ(::write(fd, partial_header, sizeof(partial_header)),
            static_cast<ssize_t>(sizeof(partial_header)));
  ::close(fd);

  ShardedKV kv;
  uint64_t last_applied_index = 0;
  ASSERT_TRUE(RecoverFromWal(dir.path(), kv, &last_applied_index).ok());
  EXPECT_EQ(last_applied_index, 1u);
  EXPECT_TRUE(kv.Get("k1").ok());
}

TEST(WalTest, RecoverDetectsCrcCorruption) {
  TempDataDir dir;
  {
    WalWriter writer(dir.path());
    WalRecord set1;
    set1.op = WalOp::kSet;
    set1.key = "k1";
    set1.value = "v1";
    Result<uint64_t> appended = writer.Append(set1);
    ASSERT_TRUE(appended.ok());
    ASSERT_TRUE(writer.Sync(appended.value()).ok());
  }

  // Flip a bit in the key bytes (offset 25: after the 25-byte fixed
  // header) without touching the CRC field, so the record is fully
  // present but no longer matches its checksum.
  const std::string wal_path = dir.path() + "/wal/wal.log";
  const int fd = ::open(wal_path.c_str(), O_RDWR);
  ASSERT_GE(fd, 0);
  uint8_t byte = 0;
  ASSERT_EQ(::pread(fd, &byte, 1, 25), 1);
  byte = static_cast<uint8_t>(byte ^ 0xFF);
  ASSERT_EQ(::pwrite(fd, &byte, 1, 25), 1);
  ::close(fd);

  ShardedKV kv;
  uint64_t last_applied_index = 0;
  const Status status = RecoverFromWal(dir.path(), kv, &last_applied_index);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), ErrorCode::kIOError);
}

}  // namespace
}  // namespace neuralkv::persistence
