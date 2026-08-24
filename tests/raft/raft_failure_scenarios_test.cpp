#include <gtest/gtest.h>

#include <array>
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
    char pattern[] = "/tmp/nkv_failure_test_XXXXXX";
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

// 3-node in-process cluster wired with FaultInjectingTransport on every
// node and all three registered with one FaultInjectionController, so a
// scenario can isolate a node from the rest (or from one specific peer)
// with a single call instead of managing per-transport state by hand.
class FailureTestCluster {
 public:
  explicit FailureTestCluster(std::vector<uint16_t> ports) {
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
      controller_.Register(node->config.local_node_id, *node->transport);
      nodes_.push_back(std::move(node));
    }
  }

  ~FailureTestCluster() {
    for (auto& node : nodes_) StopNode(*node);
  }

  std::size_t size() const { return nodes_.size(); }
  static uint32_t NodeId(std::size_t index) { return static_cast<uint32_t>(index) + 1; }

  RaftNode& Raft(std::size_t index) { return *nodes_[index]->raft; }
  persistence::DurableStorage& Storage(std::size_t index) { return *nodes_[index]->storage; }
  bool IsLive(std::size_t index) const { return !nodes_[index]->dead; }

  // Isolates node `victim` from every other live node.
  void IsolateFromAll(std::size_t victim) {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
      if (i != victim) controller_.partition(NodeId(victim), NodeId(i), true);
    }
  }

  void HealAll() { controller_.clear_faults(); }

  Status ProposeOn(std::size_t index, LogEntry entry) {
    return nodes_[index]->raft->Propose(std::move(entry));
  }

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

  // Current leader with no wait, or -1 if none/more than one right now
  // (mid-election transients are possible; callers retry).
  int CurrentLeader() {
    int leader_index = -1;
    int leader_count = 0;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
      if (nodes_[i]->dead) continue;
      if (nodes_[i]->raft->state() == RaftState::kLeader) {
        leader_index = static_cast<int>(i);
        ++leader_count;
      }
    }
    return leader_count == 1 ? leader_index : -1;
  }

  bool WaitForAppliedIndex(std::size_t index, uint64_t at_least, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (nodes_[index]->storage->last_applied_index() >= at_least) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }

  void KillNode(std::size_t index) { StopNode(*nodes_[index]); }

 private:
  struct Node {
    cluster::ClusterConfig config;
    TempDir data_dir;
    std::unique_ptr<persistence::DurableStorage> storage;
    std::unique_ptr<testing::FaultInjectingTransport> transport;
    std::unique_ptr<RaftNode> raft;
    std::unique_ptr<BlockingServer> server;
    std::thread server_thread;
    bool dead = false;
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
    node.dead = false;
  }

  static void StopNode(Node& node) {
    if (node.dead) return;
    node.raft->Stop();
    node.server->Stop();
    if (node.server_thread.joinable()) node.server_thread.join();
    node.dead = true;
  }

  testing::FaultInjectionController controller_;
  std::vector<std::unique_ptr<Node>> nodes_;
};

std::vector<uint16_t> ThreeNodePorts(uint16_t base) { return {base, static_cast<uint16_t>(base + 1),
                                                               static_cast<uint16_t>(base + 2)}; }

LogEntry SetEntry(const std::string& key, const std::string& value) {
  return LogEntry{.term = 0, .index = 0, .op = persistence::WalOp::kSet, .key = key, .value = value};
}

// --- Scenario 1: a partitioned minority follower can't block majority
// progress, and can't itself become a commit source. ----------------------

TEST(RaftFailureScenariosTest, MajorityPartitionContinues) {
  FailureTestCluster cluster(ThreeNodePorts(18751));
  const int leader = cluster.WaitForLeader(std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0);
  const std::size_t follower = static_cast<std::size_t>((leader + 1) % 3);

  cluster.IsolateFromAll(follower);

  ASSERT_TRUE(cluster.ProposeOn(static_cast<std::size_t>(leader), SetEntry("k1", "v1")).ok());
  ASSERT_TRUE(cluster.WaitForAppliedIndex(static_cast<std::size_t>(leader), 1, std::chrono::milliseconds(2000)));

  // The isolated follower can never reach a majority of votes with both
  // peers unreachable, so it can't be a commit source for anything
  // while cut off — Propose() on it must fail, and it must never reach
  // kLeader. It *will* legitimately become a candidate once its own
  // election timeout fires (that's expected — nothing tells it it's
  // isolated), possibly cycling through repeated candidacies, so its
  // exact state at any one instant isn't itself the invariant worth
  // checking.
  const Status propose_on_isolated = cluster.ProposeOn(follower, SetEntry("k2", "v2"));
  EXPECT_FALSE(propose_on_isolated.ok());
  EXPECT_NE(cluster.Raft(follower).state(), RaftState::kLeader);

  cluster.HealAll();
  ASSERT_TRUE(cluster.WaitForAppliedIndex(follower, 1, std::chrono::milliseconds(2000)))
      << "isolated follower never caught up after healing";
}

// --- Scenario 2: an isolated leader (minority side) can't commit, and
// the cluster reconverges once the partition heals. ------------------------

TEST(RaftFailureScenariosTest, MinorityPartitionCannotCommit) {
  FailureTestCluster cluster(ThreeNodePorts(18761));
  const int leader = cluster.WaitForLeader(std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0);

  cluster.IsolateFromAll(static_cast<std::size_t>(leader));

  const Status propose_on_isolated_leader =
      cluster.ProposeOn(static_cast<std::size_t>(leader), SetEntry("orphaned", "x"));
  EXPECT_FALSE(propose_on_isolated_leader.ok())
      << "an isolated leader must not be able to commit a write";

  cluster.HealAll();

  const int reconverged_leader = cluster.WaitForLeader(std::chrono::milliseconds(2000));
  ASSERT_GE(reconverged_leader, 0) << "cluster never reconverged on a single leader after healing";

  ASSERT_TRUE(cluster.ProposeOn(static_cast<std::size_t>(reconverged_leader), SetEntry("after-heal", "ok")).ok());
  for (std::size_t i = 0; i < cluster.size(); ++i) {
    ASSERT_TRUE(cluster.WaitForAppliedIndex(i, cluster.Storage(static_cast<std::size_t>(reconverged_leader))
                                                    .last_applied_index(),
                                             std::chrono::milliseconds(2000)))
        << "node " << i << " never converged after the partition healed";
  }
}

// --- Scenario 3: killing the leader mid-burst loses no write the client
// actually saw succeed. -----------------------------------------------------

TEST(RaftFailureScenariosTest, LeaderFailoverDuringWrites) {
  FailureTestCluster cluster(ThreeNodePorts(18771));
  const int initial_leader = cluster.WaitForLeader(std::chrono::milliseconds(2000));
  ASSERT_GE(initial_leader, 0);

  constexpr int kWriteCount = 50;
  std::array<bool, kWriteCount> succeeded{};
  std::atomic<int> leader_hint{initial_leader};

  std::thread writer([&] {
    int leader_idx = leader_hint.load();
    for (int i = 0; i < kWriteCount; ++i) {
      if (leader_idx < 0) {
        leader_idx = cluster.WaitForLeader(std::chrono::milliseconds(1000));
      }
      if (leader_idx < 0) {
        succeeded[static_cast<std::size_t>(i)] = false;
        continue;
      }
      const Status status =
          cluster.ProposeOn(static_cast<std::size_t>(leader_idx), SetEntry("k" + std::to_string(i),
                                                                            "v" + std::to_string(i)));
      succeeded[static_cast<std::size_t>(i)] = status.ok();
      if (!status.ok()) leader_idx = -1;  // rediscover — leadership may have moved
    }
  });

  // Let a chunk of writes land, then kill whichever node is currently
  // leader (it may not be initial_leader if an election already
  // happened by coincidence — either way, killing whoever holds it now
  // is the scenario).
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  int leader_to_kill = cluster.CurrentLeader();
  while (leader_to_kill < 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    leader_to_kill = cluster.CurrentLeader();
  }
  cluster.KillNode(static_cast<std::size_t>(leader_to_kill));

  writer.join();

  int new_leader = -1;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  while (std::chrono::steady_clock::now() < deadline) {
    for (std::size_t i = 0; i < cluster.size(); ++i) {
      if (static_cast<int>(i) == leader_to_kill) continue;
      if (cluster.IsLive(i) && cluster.Raft(i).state() == RaftState::kLeader) {
        new_leader = static_cast<int>(i);
        break;
      }
    }
    if (new_leader >= 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_GE(new_leader, 0) << "no new leader elected after killing the leader mid-burst";

  // A new leader can't directly advance commit_index_ over entries from
  // a *previous* term just because a majority already has them (Raft's
  // §5.4.2 safety rule — see raft-design.md's Commit and apply
  // section); it only does so indirectly, once it commits a fresh entry
  // in its own current term. Propose one now so every earlier entry
  // still sitting in the log (possibly including the tail of this
  // burst, if the old leader died before a post-commit heartbeat ever
  // reached this node) actually gets applied before checking anything.
  ASSERT_TRUE(cluster.ProposeOn(static_cast<std::size_t>(new_leader), SetEntry("flush", "flush")).ok());

  int ok_count = 0;
  for (int i = 0; i < kWriteCount; ++i) {
    if (!succeeded[static_cast<std::size_t>(i)]) continue;
    ++ok_count;
    const std::string key = "k" + std::to_string(i);
    const std::string expected = "v" + std::to_string(i);
    Result<std::string> value = cluster.Storage(static_cast<std::size_t>(new_leader)).Get(key);
    ASSERT_TRUE(value.ok()) << "key " << key << " returned OK to the client but is missing on the new leader";
    EXPECT_EQ(value.value(), expected);
  }
  EXPECT_GT(ok_count, 0) << "no writes succeeded before the leader was killed — scenario didn't exercise anything";
}

// --- Scenario 4: a follower that missed writes while partitioned catches
// up fully once the partition heals. ---------------------------------------

TEST(RaftFailureScenariosTest, RejoinAfterPartition) {
  FailureTestCluster cluster(ThreeNodePorts(18781));
  const int leader = cluster.WaitForLeader(std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0);
  const std::size_t follower = static_cast<std::size_t>((leader + 1) % 3);

  cluster.IsolateFromAll(follower);

  constexpr int kWriteCount = 10;
  for (int i = 0; i < kWriteCount; ++i) {
    ASSERT_TRUE(cluster
                    .ProposeOn(static_cast<std::size_t>(leader),
                               SetEntry("k" + std::to_string(i), "v" + std::to_string(i)))
                    .ok());
  }

  EXPECT_NE(cluster.Raft(static_cast<std::size_t>(leader)).replication_lag_entries(FailureTestCluster::NodeId(follower)),
            0u)
      << "the partitioned follower shouldn't have received anything yet";

  cluster.HealAll();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  bool caught_up = false;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cluster.Raft(static_cast<std::size_t>(leader))
            .replication_lag_entries(FailureTestCluster::NodeId(follower)) == 0) {
      caught_up = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_TRUE(caught_up) << "follower's replication lag never dropped to 0 after the partition healed";

  Result<std::string> last = cluster.Storage(follower).Get("k" + std::to_string(kWriteCount - 1));
  ASSERT_TRUE(last.ok());
  EXPECT_EQ(last.value(), "v" + std::to_string(kWriteCount - 1));
}

}  // namespace
}  // namespace neuralkv::raft
