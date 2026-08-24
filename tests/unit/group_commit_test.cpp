#include "persistence/wal_writer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "common/result.h"
#include "persistence/wal_record.h"

// Declared friend in wal_writer.h; defined here only, so it never links
// into any production binary.
namespace neuralkv::persistence {
uint64_t TestOnlyFsyncCount(const WalWriter& writer) {
  std::lock_guard<std::mutex> lock(*writer.mutex_);
  return writer.fsync_count_;
}
}  // namespace neuralkv::persistence

namespace neuralkv::persistence {
namespace {

class TempDataDir {
 public:
  TempDataDir() {
    char pattern[] = "/tmp/nkv_group_commit_test_XXXXXX";
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

WalRecord SetRecord(const std::string& key, const std::string& value) {
  WalRecord record;
  record.op = WalOp::kSet;
  record.key = key;
  record.value = value;
  return record;
}

TEST(GroupCommitTest, ConcurrentAppendsCoalesceIntoFewerFsyncs) {
  TempDataDir dir;
  WalWriter writer(dir.path());

  constexpr int kThreads = 20;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&writer, i] {
      Result<uint64_t> appended = writer.Append(SetRecord("k" + std::to_string(i), "v"));
      ASSERT_TRUE(appended.ok());
      ASSERT_TRUE(writer.Sync(appended.value()).ok());
    });
  }
  for (std::thread& t : threads) t.join();

  EXPECT_EQ(writer.last_index(), static_cast<uint64_t>(kThreads));
  // 20 concurrent appends batching into far fewer than 20 fsyncs is the
  // whole point; anything close to 1-per-append would mean the batching
  // window never got a chance to do its job.
  EXPECT_LT(TestOnlyFsyncCount(writer), static_cast<uint64_t>(kThreads))
      << "expected concurrent appends to coalesce into fewer fsync calls than appends";
}

TEST(GroupCommitTest, SequentialAppendsEachGetTheirOwnFsync) {
  TempDataDir dir;
  WalWriter writer(dir.path());

  // No concurrency at all: each Append+Sync pair fully completes
  // (including its own fsync) before the next one starts, so there's
  // never anything else to batch with. Group commit shouldn't invent
  // batching where none is possible.
  for (int i = 0; i < 5; ++i) {
    Result<uint64_t> appended = writer.Append(SetRecord("k" + std::to_string(i), "v"));
    ASSERT_TRUE(appended.ok());
    ASSERT_TRUE(writer.Sync(appended.value()).ok());
  }

  EXPECT_EQ(TestOnlyFsyncCount(writer), 5u);
}

TEST(GroupCommitTest, SyncReturnsOnceOwnIndexIsDurableEvenIfLaterIndicesArrive) {
  TempDataDir dir;
  WalWriter writer(dir.path());

  Result<uint64_t> first = writer.Append(SetRecord("k1", "v1"));
  ASSERT_TRUE(first.ok());

  // A second append arrives before the first's Sync() call — by the
  // time Sync(first) actually flushes, it should cover both, and
  // Sync(first) must not need to wait for a third append that hasn't
  // happened yet.
  Result<uint64_t> second = writer.Append(SetRecord("k2", "v2"));
  ASSERT_TRUE(second.ok());

  ASSERT_TRUE(writer.Sync(first.value()).ok());
  EXPECT_GE(writer.last_index(), second.value());
}

TEST(GroupCommitTest, RecordCapFlushesWithoutWaitingOutTheFullDelay) {
  TempDataDir dir;
  WalWriter writer(dir.path());

  std::atomic<bool> flushed{false};
  std::thread syncer([&] {
    Result<uint64_t> appended = writer.Append(SetRecord("k0", "v"));
    ASSERT_TRUE(appended.ok());
    ASSERT_TRUE(writer.Sync(appended.value()).ok());
    flushed.store(true);
  });

  // Feed enough concurrent appends to hit the record cap quickly; the
  // syncer's flush should complete well before the 1ms delay cap would
  // force it to, since the cap is meant to be an upper bound, not a
  // mandatory wait.
  for (int i = 1; i < kGroupCommitMaxRecords + 4; ++i) {
    writer.Append(SetRecord("k" + std::to_string(i), "v"));
  }
  syncer.join();
  EXPECT_TRUE(flushed.load());
}

}  // namespace
}  // namespace neuralkv::persistence
