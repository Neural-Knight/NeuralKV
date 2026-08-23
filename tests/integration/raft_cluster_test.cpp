#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <unistd.h>

#include "common/result.h"
#include "protocol/types.h"
#include "raft_test_support.h"
#include "test_support.h"

namespace neuralkv {
namespace {

// Fixed ports: every node's cluster config has to name all three
// addresses before any of them start, so ephemeral (--port 0) isn't an
// option here — same constraint as cluster_transport_test.cpp.
const std::vector<uint16_t> kPorts = {18701, 18702, 18703};

class RaftClusterTest : public ::testing::Test {
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

  // Repeatedly probes every live node with a SET until one accepts it
  // (i.e. is the current leader), or the deadline passes. The generous
  // deadline accounts for this test's own fork/exec/connect overhead on
  // top of Raft's sub-second election timeout.
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

TEST_F(RaftClusterTest, LeaderWriteReplicatesToFollowers) {
  const int leader = FindLeaderAndSet("k1", "v1", std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0) << "no leader accepted a write";

  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    if (static_cast<int>(i) == leader) continue;

    // GET on a follower is WRONG_LEADER by default (linearizable reads —
    // see raft_linearizable_read_test.cpp); nkv-client's --cluster-config
    // redirect follows that to the leader, which serves the value once
    // replication has caught up.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    bool caught_up = false;
    while (std::chrono::steady_clock::now() < deadline) {
      const testutil::ClientResult redirected =
          testutil::RunClient({"--port", std::to_string(nodes_[i]->port()), "--cluster-config",
                                conf_paths_[i], "get", "k1"});
      if (redirected.exit_code == 0 && redirected.stdout_output.find("v1") != std::string::npos) {
        caught_up = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(caught_up) << "follower " << i << " never caught up to the leader's write";
  }
}

TEST_F(RaftClusterTest, FollowerRedirectsClientToLeader) {
  const int leader = FindLeaderAndSet("seed", "value", std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0);

  const int follower = (leader + 1) % static_cast<int>(nodes_.size());
  Result<protocol::ClientResponse> direct = testutil::TrySendClientRequest(
      nodes_[static_cast<std::size_t>(follower)]->port(),
      protocol::ClientRequest{.request_id = 3, .opcode = protocol::Opcode::kSet, .key = "k2", .value = "v2"});
  ASSERT_TRUE(direct.ok());
  EXPECT_EQ(direct.value().status, protocol::ResponseStatus::kWrongLeader);
  EXPECT_NE(direct.value().leader_hint, 0u);

  const testutil::ClientResult redirected =
      testutil::RunClient({"--port", std::to_string(nodes_[static_cast<std::size_t>(follower)]->port()),
                            "--cluster-config", conf_paths_[static_cast<std::size_t>(follower)], "set",
                            "k2", "v2"});
  EXPECT_EQ(redirected.exit_code, 0);
  EXPECT_NE(redirected.stdout_output.find("OK"), std::string::npos);
}

TEST_F(RaftClusterTest, NewLeaderElectedAfterLeaderCrash) {
  const int leader = FindLeaderAndSet("before-crash", "still-here", std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0);

  nodes_[static_cast<std::size_t>(leader)]->Kill(SIGKILL);

  // Raft's own election timeout is well under a second (see raft/node.cpp);
  // this budget adds slack for the test harness's fork/exec/connect cost.
  const int new_leader = FindLeaderAndSet("after-crash", "elected", std::chrono::milliseconds(2500));
  ASSERT_GE(new_leader, 0) << "no new leader elected after the old leader was killed";
  EXPECT_NE(new_leader, leader);
}

}  // namespace
}  // namespace neuralkv
