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
#include "cluster/transport.h"
#include "persistence/durable_storage.h"
#include "persistence/wal_record.h"
#include "raft/node.h"
#include "raft/rpc_codec.h"
#include "server/blocking_server.h"

namespace neuralkv::raft {
namespace {

// Temp data dir for one node's WAL + persisted Raft term/vote file.
class TempDir {
 public:
  TempDir() {
    char pattern[] = "/tmp/nkv_raft_test_XXXXXX";
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

// A small in-process 3-node cluster: each "node" is exactly what
// nkv-server wires together (DurableStorage + ClusterTransport + RaftNode +
// BlockingServer), just constructed directly in this process on loopback
// ports instead of forked as a subprocess. This exercises real RaftNode
// RPC handling and real replication timing without needing the nkv-server
// binary or the client protocol at all.
class TestCluster {
 public:
  explicit TestCluster(std::vector<uint16_t> ports) {
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

  ~TestCluster() {
    for (auto& node : nodes_) StopNode(*node);
  }

  std::size_t size() const { return nodes_.size(); }

  RaftNode& Raft(std::size_t index) { return *nodes_[index]->raft; }
  persistence::DurableStorage& Storage(std::size_t index) { return *nodes_[index]->storage; }

  void KillNode(std::size_t index) { StopNode(*nodes_[index]); }

  Status ProposeOn(std::size_t index, LogEntry entry) {
    return nodes_[index]->raft->Propose(std::move(entry));
  }

  // Returns the index of the sole live leader, or -1 if there isn't
  // exactly one within timeout.
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

  // Polls until every live node's applied index reaches at_least, or
  // returns false on timeout.
  bool WaitForAppliedIndex(uint64_t at_least, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      std::size_t live_count = 0;
      std::size_t caught_up = 0;
      for (auto& node : nodes_) {
        if (node->dead) continue;
        ++live_count;
        if (node->storage->last_applied_index() >= at_least) ++caught_up;
      }
      if (live_count > 0 && caught_up == live_count) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }

 private:
  struct Node {
    cluster::ClusterConfig config;
    TempDir data_dir;
    std::unique_ptr<persistence::DurableStorage> storage;
    std::unique_ptr<cluster::ClusterTransport> transport;
    std::unique_ptr<RaftNode> raft;
    std::unique_ptr<BlockingServer> server;
    std::thread server_thread;
    bool dead = false;
  };

  static void StartNode(Node& node, uint16_t port) {
    node.storage = std::make_unique<persistence::DurableStorage>(node.data_dir.path());
    node.transport = std::make_unique<cluster::ClusterTransport>(node.config.local_node_id);
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

std::vector<uint16_t> ThreeNodePorts() { return {18601, 18602, 18603}; }

// --- Scenario 1: leader election on startup -------------------------------

TEST(RaftFigure8Test, ElectsExactlyOneLeaderOnStartup) {
  TestCluster cluster(ThreeNodePorts());
  const int leader = cluster.WaitForLeader(std::chrono::milliseconds(2000));
  EXPECT_GE(leader, 0) << "no single leader emerged";
}

// --- Scenario 2: replication commits on majority ---------------------------

TEST(RaftFigure8Test, ReplicatesAndCommitsOnMajority) {
  TestCluster cluster(ThreeNodePorts());
  const int leader = cluster.WaitForLeader(std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0);

  const Status status = cluster.ProposeOn(
      static_cast<std::size_t>(leader),
      LogEntry{.term = 0, .index = 0, .op = persistence::WalOp::kSet, .key = "k1", .value = "v1"});
  ASSERT_TRUE(status.ok()) << status.message();

  ASSERT_TRUE(cluster.WaitForAppliedIndex(1, std::chrono::milliseconds(2000)));
  for (std::size_t i = 0; i < cluster.size(); ++i) {
    Result<std::string> value = cluster.Storage(i).Get("k1");
    ASSERT_TRUE(value.ok());
    EXPECT_EQ(value.value(), "v1");
  }
}

// --- Scenario 3: leader crash triggers re-election, no committed loss -----

TEST(RaftFigure8Test, SurvivesLeaderCrashWithoutLosingCommittedEntries) {
  TestCluster cluster(ThreeNodePorts());
  const int first_leader = cluster.WaitForLeader(std::chrono::milliseconds(2000));
  ASSERT_GE(first_leader, 0);

  ASSERT_TRUE(cluster
                  .ProposeOn(static_cast<std::size_t>(first_leader),
                             LogEntry{.term = 0,
                                      .index = 0,
                                      .op = persistence::WalOp::kSet,
                                      .key = "durable",
                                      .value = "before-crash"})
                  .ok());
  ASSERT_TRUE(cluster.WaitForAppliedIndex(1, std::chrono::milliseconds(2000)));

  cluster.KillNode(static_cast<std::size_t>(first_leader));

  int new_leader = -1;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  while (std::chrono::steady_clock::now() < deadline) {
    for (std::size_t i = 0; i < cluster.size(); ++i) {
      if (static_cast<int>(i) == first_leader) continue;
      if (cluster.Raft(i).state() == RaftState::kLeader) {
        new_leader = static_cast<int>(i);
        break;
      }
    }
    if (new_leader >= 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_GE(new_leader, 0) << "no new leader elected within 2s of leader crash";
  EXPECT_NE(new_leader, first_leader);

  for (std::size_t i = 0; i < cluster.size(); ++i) {
    if (static_cast<int>(i) == first_leader) continue;
    Result<std::string> value = cluster.Storage(i).Get("durable");
    ASSERT_TRUE(value.ok()) << "surviving node " << i << " lost a committed entry";
    EXPECT_EQ(value.value(), "before-crash");
  }

  ASSERT_TRUE(cluster
                  .ProposeOn(static_cast<std::size_t>(new_leader),
                             LogEntry{.term = 0,
                                      .index = 0,
                                      .op = persistence::WalOp::kSet,
                                      .key = "after-crash",
                                      .value = "still-works"})
                  .ok());
}

// --- Scenarios 4 and 5: driven directly against one RaftNode's RPC
// handlers, with no live cluster or timers needed. ------------------------

class SingleRaftNodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config_.local_node_id = 1;
    config_.peers = {cluster::PeerInfo{.node_id = 1, .host = "127.0.0.1", .port = 1},
                      cluster::PeerInfo{.node_id = 2, .host = "127.0.0.1", .port = 2}};
    storage_ = std::make_unique<persistence::DurableStorage>(data_dir_.path());
    transport_ = std::make_unique<cluster::ClusterTransport>(1);
    node_ = std::make_unique<RaftNode>(1, config_, *storage_, *transport_);
    ASSERT_TRUE(node_->open_status().ok());
  }

  TempDir data_dir_;
  cluster::ClusterConfig config_;
  std::unique_ptr<persistence::DurableStorage> storage_;
  std::unique_ptr<cluster::ClusterTransport> transport_;
  std::unique_ptr<RaftNode> node_;
};

TEST_F(SingleRaftNodeTest, ConflictingFollowerEntryIsTruncatedAndReplaced) {
  const AppendEntriesRequest first{
      .term = 1,
      .leader_id = 9,
      .prev_log_index = 0,
      .prev_log_term = 0,
      .leader_commit = 0,
      .entries = {LogEntry{.term = 1, .index = 1, .op = persistence::WalOp::kSet, .key = "k", .value = "stale"},
                  LogEntry{.term = 1,
                           .index = 2,
                           .op = persistence::WalOp::kSet,
                           .key = "k2",
                           .value = "stale2"}}};
  const AppendEntriesResponse resp1 = node_->HandleAppendEntries(first);
  ASSERT_TRUE(resp1.success);

  // A new leader at a higher term overwrites index 2 with different
  // content; the follower must truncate its stale suffix and take the
  // new leader's version.
  const AppendEntriesRequest second{
      .term = 2,
      .leader_id = 10,
      .prev_log_index = 1,
      .prev_log_term = 1,
      .leader_commit = 2,
      .entries = {LogEntry{
          .term = 2, .index = 2, .op = persistence::WalOp::kSet, .key = "k2", .value = "fresh2"}}};
  const AppendEntriesResponse resp2 = node_->HandleAppendEntries(second);
  ASSERT_TRUE(resp2.success);
  EXPECT_EQ(resp2.term, 2u);

  Result<std::string> k = storage_->Get("k");
  ASSERT_TRUE(k.ok());
  EXPECT_EQ(k.value(), "stale");  // untouched: no conflict at index 1

  Result<std::string> k2 = storage_->Get("k2");
  ASSERT_TRUE(k2.ok());
  EXPECT_EQ(k2.value(), "fresh2");  // replaced: index 2 conflicted and was overwritten
}

TEST_F(SingleRaftNodeTest, StaleTermCandidateIsRejected) {
  const AppendEntriesRequest bump_term{
      .term = 5, .leader_id = 9, .prev_log_index = 0, .prev_log_term = 0, .leader_commit = 0, .entries = {}};
  ASSERT_TRUE(node_->HandleAppendEntries(bump_term).success);
  ASSERT_EQ(node_->current_term(), 5u);

  const RequestVoteRequest stale_candidate{
      .term = 3, .candidate_id = 42, .last_log_index = 0, .last_log_term = 0};
  const RequestVoteResponse resp = node_->HandleRequestVote(stale_candidate);
  EXPECT_FALSE(resp.vote_granted);
  EXPECT_EQ(resp.term, 5u);
}

}  // namespace
}  // namespace neuralkv::raft
