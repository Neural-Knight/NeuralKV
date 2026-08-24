#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "cluster/cluster_config.h"
#include "cluster/transport.h"
#include "common/status.h"
#include "persistence/durable_storage.h"
#include "raft/log.h"
#include "raft/rpc_codec.h"
#include "raft/types.h"

namespace neuralkv::raft {

// One node's Raft state machine: leader election, log replication, and commit/
// apply, per the Ongaro thesis. A background thread runs elections and heartbeats;
// RPC handlers run on whichever thread serves them, so all state access goes through mutex_.
class RaftNode {
 public:
  RaftNode(uint32_t local_node_id, const cluster::ClusterConfig& config,
           persistence::DurableStorage& storage, cluster::ClusterTransport& transport);
  ~RaftNode();

  RaftNode(const RaftNode&) = delete;
  RaftNode& operator=(const RaftNode&) = delete;

  // Status of loading persisted term/vote and the WAL-backed log at
  // construction time; Start() refuses to run if this isn't Ok().
  const Status& open_status() const { return open_status_; }

  void Start();
  void Stop();

  // Leader-only: appends entry, replicates it, and blocks until committed and
  // applied. Returns kInvalidArgument if this node isn't the leader (including
  // losing leadership mid-call) — callers map that to a client-facing WRONG_LEADER.
  Status Propose(LogEntry entry);

  // read_index-style linearizable-read check: one round of empty AppendEntries to
  // every peer, true only if a majority (including self) confirms the same current
  // term — meaning no other leader has been elected since. False means not safe to read here.
  bool ConfirmLeadershipQuorum();

  RaftState state() const;
  uint64_t current_term() const;
  uint64_t commit_index() const;
  uint64_t last_applied() const;
  uint32_t leader_id() const;  // 0 if unknown

  // commit_index() - last_applied(): entries this node has committed but
  // not yet applied to storage. Non-zero only briefly — apply runs
  // synchronously with whatever just advanced commit_index.
  uint64_t lag_entries() const;

  // Leader's last log index minus peer_id's matchIndex — how far behind that
  // follower is. 0 if peer_id is this node, or if this node isn't leader
  // (no matchIndex tracking without leadership).
  uint64_t replication_lag_entries(uint32_t peer_id) const;

  RequestVoteResponse HandleRequestVote(const RequestVoteRequest& req);
  AppendEntriesResponse HandleAppendEntries(const AppendEntriesRequest& req);

 private:
  void RunLoop();
  void StartElection();
  void ReplicateToPeer(const cluster::PeerInfo& peer);
  bool HasRecentMajorityContactLocked() const;
  void AdvanceCommitIndexLocked();
  void ApplyCommittedEntriesLocked();
  void BecomeFollowerLocked(uint64_t term);
  void BecomeLeaderLocked();
  void PersistStateLocked();
  Status LoadPersistedState();
  void ResetElectionDeadlineLocked();
  std::chrono::milliseconds RandomElectionTimeout();

  uint32_t local_node_id_;
  const cluster::ClusterConfig& config_;
  persistence::DurableStorage& storage_;
  cluster::ClusterTransport& transport_;
  Log log_;
  std::string state_path_;
  Status open_status_ = Status::Ok();

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  RaftState state_ = RaftState::kFollower;
  uint64_t current_term_ = 0;
  uint32_t voted_for_ = 0;
  uint32_t leader_id_ = 0;
  uint64_t commit_index_ = 0;
  std::chrono::steady_clock::time_point election_deadline_;
  std::chrono::steady_clock::time_point next_heartbeat_;
  std::unordered_map<uint32_t, uint64_t> next_index_;
  std::unordered_map<uint32_t, uint64_t> match_index_;
  // Last time each peer acked AppendEntries in the current term — cleared on
  // every leadership/term change. ConfirmLeadershipQuorum uses this to skip its
  // own round trip when heartbeats already established recent majority contact.
  std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> last_ack_time_;

  std::thread run_thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> started_{false};
};

}  // namespace neuralkv::raft
