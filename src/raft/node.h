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

// One node's Raft state machine: leader election, log replication, and
// commit/apply, per the Ongaro thesis. Owns a background thread that runs
// the election timer (follower/candidate) and the heartbeat/replication
// loop (leader). RequestVote/AppendEntries handlers are called synchronously
// from whichever thread is serving that inbound cluster RPC, so all state
// access goes through mutex_.
//
// propose() blocks the calling client-handler thread until its entry is
// committed and applied (or it times out, or leadership is lost) — cheap
// with a threadpool of workers, but note it also blocks the single epoll
// event-loop thread if that's the server's --io mode, the same tradeoff
// DurableStorage's fsync already carries there.
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

  // Leader-only: appends entry to the local log, replicates it, and blocks
  // until it's committed and applied to storage. Returns an error with
  // ErrorCode::kInvalidArgument if this node isn't the leader (including
  // losing leadership while the call was in flight) — callers map that
  // specifically to a client-facing WRONG_LEADER, distinct from any other
  // failure.
  Status Propose(LogEntry entry);

  // read_index-style linearizable-read check: sends one round of empty
  // AppendEntries to every peer and returns true only if a majority
  // (including this node) confirms the same current term. A true result
  // means no other leader has been elected since this round started, so
  // this node's local state already reflects every entry any client could
  // have observed as committed. Returns false (and may step down) if this
  // node isn't leader, loses leadership mid-check, or can't reach a
  // majority — callers should treat false as "not safe to read here".
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

  // Leader's last log index minus peer_id's matchIndex — how far behind
  // that follower's replicated log is, from this node's point of view.
  // 0 if peer_id is this node itself, or if this node isn't leader (a
  // follower/candidate has no matchIndex tracking for anyone).
  uint64_t replication_lag_entries(uint32_t peer_id) const;

  RequestVoteResponse HandleRequestVote(const RequestVoteRequest& req);
  AppendEntriesResponse HandleAppendEntries(const AppendEntriesRequest& req);

 private:
  void RunLoop();
  void StartElection();
  void ReplicateToPeer(const cluster::PeerInfo& peer);
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

  std::thread run_thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> started_{false};
};

}  // namespace neuralkv::raft
