#pragma once

#include "persistence/durable_storage.h"
#include "protocol/types.h"
#include "raft/node.h"

namespace neuralkv {

// Applies a decoded client request to durable storage and builds the
// response, and answers node-to-node cluster RPCs (liveness ping, and —
// when raft is set — Raft's RequestVote/AppendEntries). Owns no state
// beyond the references passed in.
//
// Without a RaftNode, the handler behaves exactly as a single node always
// has: every write applies directly to storage. With one, only the current
// Raft leader accepts SET/DELETE — a follower (or a leader that loses the
// role mid-write) rejects with kWrongLeader and the current leader's node
// id. GET is always served from local storage regardless of role — a
// follower's log can lag the leader's until it catches up, so this is a
// stale read on a follower, not a redirect; linearizable reads are future
// work.
class RequestHandler {
 public:
  explicit RequestHandler(persistence::DurableStorage& storage, raft::RaftNode* raft = nullptr);

  protocol::ClientResponse Handle(const protocol::ClientRequest& req);

  // Answers a node-to-node RPC: Ping directly, RequestVote/AppendEntries by
  // decoding into raft and re-encoding its response.
  protocol::ClusterResponse HandleCluster(const protocol::ClusterRequest& req);

 private:
  bool IsLeader() const;

  persistence::DurableStorage& storage_;
  raft::RaftNode* raft_;
};

}  // namespace neuralkv
