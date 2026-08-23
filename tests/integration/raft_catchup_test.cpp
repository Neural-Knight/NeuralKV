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

const std::vector<uint16_t> kPorts = {18731, 18732, 18733};
constexpr int kWriteCount = 100;

int FindLeaderAndSet(std::vector<std::unique_ptr<testutil::RaftNodeProcess>>& nodes,
                      const std::string& key, const std::string& value,
                      std::chrono::milliseconds deadline) {
  const auto until = std::chrono::steady_clock::now() + deadline;
  while (std::chrono::steady_clock::now() < until) {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if (!nodes[i]->alive()) continue;
      Result<protocol::ClientResponse> resp = testutil::TrySendClientRequest(
          nodes[i]->port(), protocol::ClientRequest{
                                 .request_id = 1, .opcode = protocol::Opcode::kSet, .key = key, .value = value});
      if (resp.ok() && resp.value().status == protocol::ResponseStatus::kOk) {
        return static_cast<int>(i);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return -1;
}

class RaftCatchupTest : public ::testing::Test {
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

  testutil::TempDataDir config_dir_;
  std::vector<std::string> conf_paths_;
  std::vector<std::unique_ptr<testutil::TempDataDir>> data_dirs_;
  std::vector<std::unique_ptr<testutil::RaftNodeProcess>> nodes_;
};

// A follower that misses a batch of writes while stopped must replay them
// all from the leader's log after it rejoins — no entry lost, no entry
// applied out of order. The restarted follower runs with
// --allow-stale-reads so its own local catch-up is directly observable
// over GET; a normal (linearizable) follower would just answer
// WRONG_LEADER regardless of how caught up it actually is.
TEST_F(RaftCatchupTest, FollowerCatchesUpAfterRestart) {
  const int leader = FindLeaderAndSet(nodes_, "seed", "seed-value", std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0) << "no leader accepted a write";
  const std::size_t follower = static_cast<std::size_t>((leader + 1) % static_cast<int>(nodes_.size()));

  nodes_[follower]->Kill(SIGTERM);

  for (int i = 0; i < kWriteCount; ++i) {
    Result<protocol::ClientResponse> resp = testutil::TrySendClientRequest(
        nodes_[static_cast<std::size_t>(leader)]->port(),
        protocol::ClientRequest{.request_id = static_cast<uint64_t>(10 + i),
                                 .opcode = protocol::Opcode::kSet,
                                 .key = "k" + std::to_string(i),
                                 .value = "v" + std::to_string(i)});
    ASSERT_TRUE(resp.ok());
    ASSERT_EQ(resp.value().status, protocol::ResponseStatus::kOk) << "write " << i << " rejected";
  }

  const uint32_t follower_node_id = static_cast<uint32_t>(follower) + 1;
  nodes_[follower] = std::make_unique<testutil::RaftNodeProcess>(
      follower_node_id, kPorts[follower], conf_paths_[follower], data_dirs_[follower]->path(),
      std::vector<std::string>{"--allow-stale-reads"});
  ASSERT_NE(nodes_[follower]->port(), 0);

  const std::string last_key = "k" + std::to_string(kWriteCount - 1);
  const std::string last_value = "v" + std::to_string(kWriteCount - 1);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
  bool caught_up = false;
  while (std::chrono::steady_clock::now() < deadline) {
    Result<protocol::ClientResponse> resp = testutil::TrySendClientRequest(
        nodes_[follower]->port(),
        protocol::ClientRequest{.request_id = 999, .opcode = protocol::Opcode::kGet, .key = last_key, .value = ""});
    if (resp.ok() && resp.value().status == protocol::ResponseStatus::kOk &&
        resp.value().value == last_value) {
      caught_up = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  EXPECT_TRUE(caught_up) << "restarted follower never caught up to the leader's last write";
}

}  // namespace
}  // namespace neuralkv
