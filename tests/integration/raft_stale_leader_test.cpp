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

const std::vector<uint16_t> kPorts = {18711, 18712, 18713};

// Repeatedly probes every live node with a SET until one accepts it, or
// the deadline passes.
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

class RaftStaleLeaderTest : public ::testing::Test {
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

// A leader stranded without a majority (followers killed) can still believe
// it's leader, but must not be able to commit anything — Propose() can never
// reach AdvanceCommitIndexLocked's majority threshold with zero live peers.
TEST_F(RaftStaleLeaderTest, MinorityLeaderCannotCommitWrites) {
  const int leader = FindLeaderAndSet(nodes_, "seed", "value", std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0) << "no leader accepted a write";

  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    if (static_cast<int>(i) != leader) nodes_[i]->Kill(SIGKILL);
  }

  // Give the isolated leader a moment to notice it's alone (it won't —
  // there's no failure detector on the leader side, just an inability to
  // reach quorum) and confirm the write it attempts next never commits.
  Result<protocol::ClientResponse> resp = testutil::TrySendClientRequest(
      nodes_[static_cast<std::size_t>(leader)]->port(),
      protocol::ClientRequest{
          .request_id = 2, .opcode = protocol::Opcode::kSet, .key = "orphaned", .value = "x"});

  // Propose() times out after 3s (see kProposeTimeout in node.cpp) and surfaces
  // as an internal error rather than OK — either the round trip fails outright
  // or succeeds with a non-kOk response. Never kOk.
  if (resp.ok()) {
    EXPECT_NE(resp.value().status, protocol::ResponseStatus::kOk);
  }

  // And the write never actually applied on the stranded leader either —
  // committing requires a majority, which no longer exists.
  Result<protocol::ClientResponse> get = testutil::TrySendClientRequest(
      nodes_[static_cast<std::size_t>(leader)]->port(),
      protocol::ClientRequest{.request_id = 3, .opcode = protocol::Opcode::kGet, .key = "orphaned", .value = ""});
  if (get.ok()) {
    EXPECT_NE(get.value().status, protocol::ResponseStatus::kOk);
  }
}

}  // namespace
}  // namespace neuralkv
