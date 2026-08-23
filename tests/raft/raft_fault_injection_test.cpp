#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "cluster/cluster_config.h"
#include "persistence/durable_storage.h"
#include "persistence/wal_record.h"
#include "raft/node.h"
#include "server/blocking_server.h"
#include "testing/fault_injection.h"

namespace neuralkv::raft {
namespace {

class TempDir {
 public:
  TempDir() {
    char pattern[] = "/tmp/nkv_fault_test_XXXXXX";
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

// Same 3-node in-process wiring as raft_figure8_test.cpp's TestCluster,
// except each node's transport is a FaultInjectingTransport so a test can
// isolate one node from the rest and later heal the partition.
class FaultInjectedCluster {
 public:
  explicit FaultInjectedCluster(std::vector<uint16_t> ports) {
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

  ~FaultInjectedCluster() {
    for (auto& node : nodes_) StopNode(*node);
  }

  std::size_t size() const { return nodes_.size(); }
  RaftNode& Raft(std::size_t index) { return *nodes_[index]->raft; }
  persistence::DurableStorage& Storage(std::size_t index) { return *nodes_[index]->storage; }

  // Fully isolates node `victim` from every other node: drops victim's
  // outbound RPCs to everyone, and everyone else's outbound RPCs to
  // victim.
  void Partition(std::size_t victim) {
    const uint32_t victim_id = static_cast<uint32_t>(victim) + 1;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
      if (i == victim) continue;
      const uint32_t other_id = static_cast<uint32_t>(i) + 1;
      nodes_[victim]->transport->set_drop_outbound_to(other_id, true);
      nodes_[i]->transport->set_drop_outbound_to(victim_id, true);
    }
  }

  void HealPartition(std::size_t victim) {
    const uint32_t victim_id = static_cast<uint32_t>(victim) + 1;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
      if (i == victim) continue;
      const uint32_t other_id = static_cast<uint32_t>(i) + 1;
      nodes_[victim]->transport->set_drop_outbound_to(other_id, false);
      nodes_[i]->transport->set_drop_outbound_to(victim_id, false);
    }
  }

  Status ProposeOn(std::size_t index, LogEntry entry) {
    return nodes_[index]->raft->Propose(std::move(entry));
  }

  int WaitForLeader(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      int leader_index = -1;
      int leader_count = 0;
      for (std::size_t i = 0; i < nodes_.size(); ++i) {
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

  bool WaitForAppliedIndex(std::size_t index, uint64_t at_least, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (nodes_[index]->storage->last_applied_index() >= at_least) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }

 private:
  struct Node {
    cluster::ClusterConfig config;
    TempDir data_dir;
    std::unique_ptr<persistence::DurableStorage> storage;
    std::unique_ptr<testing::FaultInjectingTransport> transport;
    std::unique_ptr<RaftNode> raft;
    std::unique_ptr<BlockingServer> server;
    std::thread server_thread;
  };

  static void StartNode(Node& node, uint16_t port) {
    node.storage = std::make_unique<persistence::DurableStorage>(node.data_dir.path());
    node.transport = std::make_unique<testing::FaultInjectingTransport>(node.config.local_node_id);
    node.raft = std::make_unique<RaftNode>(node.config.local_node_id, node.config, *node.storage,
                                            *node.transport);
    ASSERT_TRUE(node.raft->open_status().ok()) << node.raft->open_status().message();
    node.server = std::make_unique<BlockingServer>("127.0.0.1", port, *node.storage, node.raft.get());
    node.raft->Start();
    node.server_thread = std::thread([s = node.server.get()] { s->Run(); });
  }

  static void StopNode(Node& node) {
    node.raft->Stop();
    node.server->Stop();
    if (node.server_thread.joinable()) node.server_thread.join();
  }

  std::vector<std::unique_ptr<Node>> nodes_;
};

TEST(RaftFaultInjectionTest, DelayedFollowerCatchesUpAfterPartitionHeals) {
  FaultInjectedCluster cluster({18741, 18742, 18743});
  const int leader = cluster.WaitForLeader(std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0);
  const std::size_t victim = static_cast<std::size_t>((leader + 1) % static_cast<int>(cluster.size()));

  cluster.Partition(victim);

  // The remaining two nodes (leader + one follower) are still a
  // majority, so writes keep committing while the third is isolated.
  ASSERT_TRUE(cluster
                  .ProposeOn(static_cast<std::size_t>(leader),
                             LogEntry{.term = 0,
                                      .index = 0,
                                      .op = persistence::WalOp::kSet,
                                      .key = "during-partition",
                                      .value = "committed-without-victim"})
                  .ok());

  const uint64_t committed_index =
      cluster.Storage(static_cast<std::size_t>(leader)).last_applied_index();
  EXPECT_FALSE(cluster.WaitForAppliedIndex(victim, committed_index, std::chrono::milliseconds(300)))
      << "partitioned node should not have received the write";

  cluster.HealPartition(victim);

  ASSERT_TRUE(cluster.WaitForAppliedIndex(victim, committed_index, std::chrono::milliseconds(3000)))
      << "victim never caught up after the partition healed";

  Result<std::string> value = cluster.Storage(victim).Get("during-partition");
  ASSERT_TRUE(value.ok());
  EXPECT_EQ(value.value(), "committed-without-victim");
}

}  // namespace
}  // namespace neuralkv::raft
