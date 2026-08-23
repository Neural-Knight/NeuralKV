#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "common/result.h"
#include "protocol/types.h"
#include "raft_test_support.h"
#include "test_support.h"

namespace neuralkv {
namespace {

const std::vector<uint16_t> kPorts = {18721, 18722, 18723};

class RaftLinearizableReadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    for (uint32_t id = 1; id <= 3; ++id) {
      conf_paths_.push_back(testutil::WriteClusterConfig(config_dir_.path(), id, kPorts));
    }
    for (uint32_t id = 1; id <= 3; ++id) {
      data_dirs_.push_back(std::make_unique<testutil::TempDataDir>());
      nodes_.push_back(std::make_unique<testutil::RaftNodeProcess>(
          id, kPorts[id - 1], conf_paths_[id - 1], data_dirs_.back()->path()));
      ASSERT_NE(nodes_.back()->port(), 0);
    }
  }

  void TearDown() override {
    nodes_.clear();
    for (const std::string& path : conf_paths_) ::unlink(path.c_str());
  }

  int FindLeaderAndSet(const std::string& key, const std::string& value,
                        std::chrono::milliseconds deadline) {
    const auto until = std::chrono::steady_clock::now() + deadline;
    while (std::chrono::steady_clock::now() < until) {
      for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (!nodes_[i]->alive()) continue;
        Result<protocol::ClientResponse> resp = testutil::TrySendClientRequest(
            nodes_[i]->port(), protocol::ClientRequest{
                                    .request_id = 1, .opcode = protocol::Opcode::kSet, .key = key, .value = value});
        if (resp.ok() && resp.value().status == protocol::ResponseStatus::kOk) {
          return static_cast<int>(i);
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return -1;
  }

  testutil::TempDataDir config_dir_;
  std::vector<std::string> conf_paths_;
  std::vector<std::unique_ptr<testutil::TempDataDir>> data_dirs_;
  std::vector<std::unique_ptr<testutil::RaftNodeProcess>> nodes_;
};

TEST_F(RaftLinearizableReadTest, FollowerGetIsWrongLeaderRegardlessOfCatchUp) {
  const int leader = FindLeaderAndSet("k1", "v1", std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0);
  const int follower = (leader + 1) % static_cast<int>(nodes_.size());

  // Immediately after the write: the follower may not have replicated it
  // yet, but even once it does (checked below), GET still isn't served
  // locally — a follower never confirms quorum on its own.
  Result<protocol::ClientResponse> immediate = testutil::TrySendClientRequest(
      nodes_[static_cast<std::size_t>(follower)]->port(),
      protocol::ClientRequest{.request_id = 2, .opcode = protocol::Opcode::kGet, .key = "k1", .value = ""});
  ASSERT_TRUE(immediate.ok());
  EXPECT_EQ(immediate.value().status, protocol::ResponseStatus::kWrongLeader);
  EXPECT_NE(immediate.value().leader_hint, 0u);

  // Give replication plenty of time to catch up, then check again.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  Result<protocol::ClientResponse> after_catchup = testutil::TrySendClientRequest(
      nodes_[static_cast<std::size_t>(follower)]->port(),
      protocol::ClientRequest{.request_id = 3, .opcode = protocol::Opcode::kGet, .key = "k1", .value = ""});
  ASSERT_TRUE(after_catchup.ok());
  EXPECT_EQ(after_catchup.value().status, protocol::ResponseStatus::kWrongLeader);
  EXPECT_NE(after_catchup.value().leader_hint, 0u);
}

TEST_F(RaftLinearizableReadTest, LeaderGetSucceedsAfterQuorumCheck) {
  const int leader = FindLeaderAndSet("k2", "v2", std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0);

  Result<protocol::ClientResponse> resp = testutil::TrySendClientRequest(
      nodes_[static_cast<std::size_t>(leader)]->port(),
      protocol::ClientRequest{.request_id = 4, .opcode = protocol::Opcode::kGet, .key = "k2", .value = ""});
  ASSERT_TRUE(resp.ok());
  EXPECT_EQ(resp.value().status, protocol::ResponseStatus::kOk);
  EXPECT_EQ(resp.value().value, "v2");
}

TEST_F(RaftLinearizableReadTest, ClientRedirectFollowsGetToLeader) {
  const int leader = FindLeaderAndSet("k3", "v3", std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0);
  const int follower = (leader + 1) % static_cast<int>(nodes_.size());

  const testutil::ClientResult redirected =
      testutil::RunClient({"--port", std::to_string(nodes_[static_cast<std::size_t>(follower)]->port()),
                            "--cluster-config", conf_paths_[static_cast<std::size_t>(follower)], "get",
                            "k3"});
  EXPECT_EQ(redirected.exit_code, 0);
  EXPECT_NE(redirected.stdout_output.find("v3"), std::string::npos);
}

}  // namespace
}  // namespace neuralkv
