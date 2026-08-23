#include <gtest/gtest.h>

#include <atomic>
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

// Disjoint from every other integration test's fixed-port range.
const std::vector<uint16_t> kPorts = {18801, 18802, 18803};

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

class RaftFailureIntegrationTest : public ::testing::Test {
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

TEST_F(RaftFailureIntegrationTest, LeaderKillUnderLoad) {
  const int leader = FindLeaderAndSet(nodes_, "seed", "seed-value", std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0) << "no leader accepted a write";

  std::atomic<bool> stop_load{false};
  std::atomic<int> load_ok_count{0};
  std::thread load([&] {
    int i = 0;
    while (!stop_load.load()) {
      Result<protocol::ClientResponse> resp = testutil::TrySendClientRequest(
          nodes_[static_cast<std::size_t>(leader)]->port(),
          protocol::ClientRequest{.request_id = static_cast<uint64_t>(100 + i),
                                   .opcode = protocol::Opcode::kSet,
                                   .key = "load" + std::to_string(i % 20),
                                   .value = "v" + std::to_string(i)});
      if (resp.ok() && resp.value().status == protocol::ResponseStatus::kOk) load_ok_count.fetch_add(1);
      ++i;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  nodes_[static_cast<std::size_t>(leader)]->Kill(SIGKILL);

  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  stop_load.store(true);
  load.join();

  EXPECT_GT(load_ok_count.load(), 0) << "no write succeeded before the leader was killed";

  const int new_leader =
      FindLeaderAndSet(nodes_, "after-failover", "elected", std::chrono::milliseconds(2000));
  ASSERT_GE(new_leader, 0) << "no new leader elected within budget after the kill";
  EXPECT_NE(new_leader, leader);
}

// A minimal client speaking the wire protocol directly: sends to whatever
// port it currently believes is leader, follows exactly one WRONG_LEADER
// redirect (nkv-client's own policy), and remembers the redirect target.
class SubprocessClient {
 public:
  SubprocessClient(std::vector<uint16_t> ports, uint16_t initial_port)
      : ports_(std::move(ports)), port_(initial_port) {}

  Result<protocol::ClientResponse> Send(const protocol::ClientRequest& req) {
    Result<protocol::ClientResponse> resp = testutil::TrySendClientRequest(port_, req);
    if (resp.ok() && resp.value().status == protocol::ResponseStatus::kWrongLeader &&
        resp.value().leader_hint != 0 && resp.value().leader_hint <= ports_.size()) {
      port_ = ports_[resp.value().leader_hint - 1];
      resp = testutil::TrySendClientRequest(port_, req);
    }
    return resp;
  }

 private:
  std::vector<uint16_t> ports_;
  uint16_t port_;
};

TEST_F(RaftFailureIntegrationTest, ConcurrentClientsSurviveLeaderChange) {
  const int leader = FindLeaderAndSet(nodes_, "seed", "seed-value", std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0);

  constexpr int kClientCount = 4;
  std::atomic<bool> stop{false};
  std::atomic<bool> monotonicity_violation{false};

  std::vector<std::thread> clients;
  for (int c = 0; c < kClientCount; ++c) {
    clients.emplace_back([&, c] {
      // Each client owns a distinct key, so any mismatch between a
      // confirmed write and a later successful read is unambiguous —
      // no other client ever writes this key.
      const std::string key = "client-key-" + std::to_string(c);
      SubprocessClient client(kPorts, kPorts[static_cast<std::size_t>(c) % kPorts.size()]);
      std::string last_confirmed;
      int i = 0;
      while (!stop.load()) {
        const std::string value = "v" + std::to_string(i);
        Result<protocol::ClientResponse> set_resp = client.Send(protocol::ClientRequest{
            .request_id = static_cast<uint64_t>(i), .opcode = protocol::Opcode::kSet, .key = key, .value = value});
        if (set_resp.ok() && set_resp.value().status == protocol::ResponseStatus::kOk) {
          last_confirmed = value;
        }

        Result<protocol::ClientResponse> get_resp = client.Send(protocol::ClientRequest{
            .request_id = static_cast<uint64_t>(i), .opcode = protocol::Opcode::kGet, .key = key, .value = ""});
        if (get_resp.ok() && get_resp.value().status == protocol::ResponseStatus::kOk &&
            !last_confirmed.empty() && get_resp.value().value != last_confirmed) {
          monotonicity_violation.store(true);
        }
        ++i;
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  nodes_[static_cast<std::size_t>(leader)]->Kill(SIGKILL);
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  stop.store(true);
  for (std::thread& c : clients) c.join();

  EXPECT_FALSE(monotonicity_violation.load())
      << "a client read back a value older than its own last confirmed write";
}

}  // namespace
}  // namespace neuralkv
