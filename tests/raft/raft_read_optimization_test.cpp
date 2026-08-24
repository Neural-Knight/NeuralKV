#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "cluster/cluster_config.h"
#include "cluster/transport.h"
#include "persistence/durable_storage.h"
#include "raft/node.h"
#include "server/blocking_server.h"

namespace neuralkv::raft {
namespace {

class TempDir {
 public:
  TempDir() {
    char pattern[] = "/tmp/nkv_read_opt_test_XXXXXX";
    const char* dir = ::mkdtemp(pattern);
    if (dir == nullptr) {
      std::perror("mkdtemp");
      std::abort();
    }
    path_ = dir;
  }

  ~TempDir() {
    ::unlink((path_ + "/wal/wal.log").c_str());
    ::rmdir((path_ + "/wal").c_str());
    ::unlink((path_ + "/raft/state.bin").c_str());
    ::rmdir((path_ + "/raft").c_str());
    ::rmdir(path_.c_str());
  }

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

// Counts every outbound RPC without altering behavior — plain pass-through to
// the real transport. Lets a test tell "skipped its round trip" apart from "ran
// one" without inspecting RaftNode's private state.
class CountingTransport : public cluster::ClusterTransport {
 public:
  using cluster::ClusterTransport::ClusterTransport;

  Result<protocol::ClusterResponse> SendRpc(const cluster::PeerInfo& peer,
                                             const protocol::ClusterRequest& req) override {
    call_count.fetch_add(1);
    return cluster::ClusterTransport::SendRpc(peer, req);
  }

  std::atomic<int> call_count{0};
};

class ReadOptimizationCluster {
 public:
  explicit ReadOptimizationCluster(std::vector<uint16_t> ports) {
    std::vector<cluster::PeerInfo> peers;
    for (std::size_t i = 0; i < ports.size(); ++i) {
      peers.push_back(cluster::PeerInfo{
          .node_id = static_cast<uint32_t>(i + 1), .host = "127.0.0.1", .port = ports[i]});
    }
    for (std::size_t i = 0; i < ports.size(); ++i) {
      auto node = std::make_unique<Node>();
      node->config.local_node_id = static_cast<uint32_t>(i + 1);
      node->config.peers = peers;
      StartNode(*node, ports[i]);
      nodes_.push_back(std::move(node));
    }
  }

  ~ReadOptimizationCluster() {
    for (auto& node : nodes_) StopNode(*node);
  }

  std::size_t size() const { return nodes_.size(); }
  RaftNode& Raft(std::size_t index) { return *nodes_[index]->raft; }
  CountingTransport& Transport(std::size_t index) { return *nodes_[index]->transport; }
  void StopNodeAt(std::size_t index) { StopNode(*nodes_[index]); }

  int WaitForLeader(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      int leader_index = -1;
      int leader_count = 0;
      for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i]->dead) continue;
        if (nodes_[i]->raft->state() == RaftState::kLeader) {
          leader_index = static_cast<int>(i);
          ++leader_count;
        }
      }
      if (leader_count == 1) return leader_index;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return -1;
  }

 private:
  struct Node {
    cluster::ClusterConfig config;
    TempDir data_dir;
    std::unique_ptr<persistence::DurableStorage> storage;
    std::unique_ptr<CountingTransport> transport;
    std::unique_ptr<RaftNode> raft;
    std::unique_ptr<BlockingServer> server;
    std::thread server_thread;
    bool dead = false;
  };

  static void StartNode(Node& node, uint16_t port) {
    node.storage = std::make_unique<persistence::DurableStorage>(node.data_dir.path());
    node.transport = std::make_unique<CountingTransport>(node.config.local_node_id);
    node.raft = std::make_unique<RaftNode>(node.config.local_node_id, node.config, *node.storage,
                                            *node.transport);
    ASSERT_TRUE(node.raft->open_status().ok()) << node.raft->open_status().message();
    node.server = std::make_unique<BlockingServer>("127.0.0.1", port, *node.storage, node.raft.get());
    node.raft->Start();
    node.server_thread = std::thread([s = node.server.get()] { s->Run(); });
    node.dead = false;
  }

  static void StopNode(Node& node) {
    if (node.dead) return;
    node.raft->Stop();
    node.server->Stop();
    if (node.server_thread.joinable()) node.server_thread.join();
    node.dead = true;
  }

  std::vector<std::unique_ptr<Node>> nodes_;
};

TEST(RaftReadOptimizationTest, SkipsQuorumRoundTripAfterRecentHeartbeats) {
  ReadOptimizationCluster cluster({18761, 18762, 18763});
  const int leader = cluster.WaitForLeader(std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0);

  // Prime the amortization cache with one explicit round instead of sleeping
  // and hoping a background heartbeat landed — under full-suite load a fixed
  // sleep isn't reliable. This call's own round trip establishes fresh contact.
  ASSERT_TRUE(cluster.Raft(static_cast<std::size_t>(leader)).ConfirmLeadershipQuorum());

  // A single before/after bracket races the leader's own background heartbeat
  // loop, which shares this counter. Looping many confirmations right after the
  // priming call makes the signal robust: at most one or two heartbeats can land in the loop, but a broken amortization would add one per call.
  constexpr int kChecks = 50;
  const int calls_before = cluster.Transport(static_cast<std::size_t>(leader)).call_count.load();
  for (int i = 0; i < kChecks; ++i) {
    EXPECT_TRUE(cluster.Raft(static_cast<std::size_t>(leader)).ConfirmLeadershipQuorum());
  }
  const int calls_after = cluster.Transport(static_cast<std::size_t>(leader)).call_count.load();

  EXPECT_LT(calls_after - calls_before, kChecks)
      << "most read confirmations with fresh heartbeat contact should skip their round trip";
}

TEST(RaftReadOptimizationTest, RunsFullRoundAfterHeartbeatsStopLanding) {
  ReadOptimizationCluster cluster({18771, 18772, 18773});
  const int leader = cluster.WaitForLeader(std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ASSERT_TRUE(cluster.Raft(static_cast<std::size_t>(leader)).ConfirmLeadershipQuorum())
      << "sanity check: quorum is confirmable before followers go away";

  // Take down both followers so every subsequent heartbeat fails to
  // land an ack, and wait past the heartbeat interval so the last real
  // acks age out of the "recent" window.
  for (std::size_t i = 0; i < cluster.size(); ++i) {
    if (static_cast<int>(i) != leader) cluster.StopNodeAt(i);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  const int calls_before = cluster.Transport(static_cast<std::size_t>(leader)).call_count.load();
  EXPECT_FALSE(cluster.Raft(static_cast<std::size_t>(leader)).ConfirmLeadershipQuorum())
      << "no majority is reachable with both followers down";
  const int calls_after = cluster.Transport(static_cast<std::size_t>(leader)).call_count.load();

  EXPECT_GT(calls_after, calls_before)
      << "a stale/failed heartbeat history should force a real quorum round trip";
}

}  // namespace
}  // namespace neuralkv::raft
