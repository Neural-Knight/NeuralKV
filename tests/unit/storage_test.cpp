#include "storage/sharded_kv.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

namespace neuralkv {
namespace {

TEST(ShardedKVTest, SetThenGetReturnsSameValue) {
  ShardedKV kv;
  ASSERT_TRUE(kv.Set("k1", "v1").ok());

  Result<std::string> result = kv.Get("k1");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), "v1");
}

TEST(ShardedKVTest, GetOnMissingKeyReturnsNotFound) {
  ShardedKV kv;
  Result<std::string> result = kv.Get("missing");
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), ErrorCode::kNotFound);
}

TEST(ShardedKVTest, SetOverwritesExistingValue) {
  ShardedKV kv;
  ASSERT_TRUE(kv.Set("k1", "v1").ok());
  ASSERT_TRUE(kv.Set("k1", "v2").ok());

  Result<std::string> result = kv.Get("k1");
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), "v2");
}

TEST(ShardedKVTest, DeleteRemovesKey) {
  ShardedKV kv;
  ASSERT_TRUE(kv.Set("k1", "v1").ok());
  ASSERT_TRUE(kv.Delete("k1").ok());

  Result<std::string> result = kv.Get("k1");
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().code(), ErrorCode::kNotFound);
}

TEST(ShardedKVTest, DeleteOnMissingKeyReturnsNotFound) {
  ShardedKV kv;
  Status status = kv.Delete("missing");
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.code(), ErrorCode::kNotFound);
}

TEST(ShardedKVTest, EmptyKeyRejectedOnAllOperations) {
  ShardedKV kv;

  Status set_status = kv.Set("", "v1");
  EXPECT_FALSE(set_status.ok());
  EXPECT_EQ(set_status.code(), ErrorCode::kInvalidArgument);

  Result<std::string> get_result = kv.Get("");
  EXPECT_FALSE(get_result.ok());
  EXPECT_EQ(get_result.status().code(), ErrorCode::kInvalidArgument);

  Status delete_status = kv.Delete("");
  EXPECT_FALSE(delete_status.ok());
  EXPECT_EQ(delete_status.code(), ErrorCode::kInvalidArgument);
}

TEST(ShardedKVTest, SizeIsZeroOnConstruction) {
  ShardedKV kv;
  EXPECT_EQ(kv.Size(), 0u);
}

TEST(ShardedKVTest, SizeTracksInsertsAndOverwrites) {
  ShardedKV kv;
  ASSERT_TRUE(kv.Set("k1", "v1").ok());
  EXPECT_EQ(kv.Size(), 1u);

  ASSERT_TRUE(kv.Set("k2", "v1").ok());
  EXPECT_EQ(kv.Size(), 2u);

  ASSERT_TRUE(kv.Set("k1", "v2").ok());
  EXPECT_EQ(kv.Size(), 2u);
}

TEST(ShardedKVTest, SizeDecrementsOnDelete) {
  ShardedKV kv;
  ASSERT_TRUE(kv.Set("k1", "v1").ok());
  ASSERT_TRUE(kv.Set("k2", "v1").ok());
  ASSERT_TRUE(kv.Delete("k1").ok());
  EXPECT_EQ(kv.Size(), 1u);
}

TEST(ShardedKVTest, ConcurrentOperationsPreserveInvariants) {
  constexpr int kThreadCount = 16;
  constexpr int kOpsPerThread = 10000;

  ShardedKV kv;
  std::vector<std::thread> workers;
  workers.reserve(kThreadCount);

  for (int t = 0; t < kThreadCount; ++t) {
    workers.emplace_back([&kv, t]() {
      const std::string prefix = "t" + std::to_string(t) + ":";
      for (int i = 0; i < kOpsPerThread; ++i) {
        const std::string key = prefix + std::to_string(i);
        ASSERT_TRUE(kv.Set(key, "v").ok());
        Result<std::string> get_result = kv.Get(key);
        ASSERT_TRUE(get_result.ok());
        EXPECT_EQ(get_result.value(), "v");
      }
      // Delete the even-indexed keys so the post-join check below has a
      // mix of present and absent keys to verify per thread.
      for (int i = 0; i < kOpsPerThread; i += 2) {
        const std::string key = prefix + std::to_string(i);
        ASSERT_TRUE(kv.Delete(key).ok());
      }
    });
  }

  for (std::thread& worker : workers) {
    worker.join();
  }

  for (int t = 0; t < kThreadCount; ++t) {
    const std::string prefix = "t" + std::to_string(t) + ":";
    for (int i = 0; i < kOpsPerThread; ++i) {
      const std::string key = prefix + std::to_string(i);
      Result<std::string> result = kv.Get(key);
      if (i % 2 == 0) {
        EXPECT_FALSE(result.ok()) << "expected " << key << " to be deleted";
      } else {
        ASSERT_TRUE(result.ok()) << "expected " << key << " to be present";
        EXPECT_EQ(result.value(), "v");
      }
    }
  }

  EXPECT_EQ(kv.Size(), static_cast<std::size_t>(kThreadCount * kOpsPerThread / 2));
}

}  // namespace
}  // namespace neuralkv
