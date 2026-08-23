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
// has: every write applies directly to storage, and GET reads local
// storage directly. With a RaftNode, only the current leader accepts
// SET/DELETE — a follower (or a leader that loses the role mid-write)
// rejects with kWrongLeader and the current leader's node id. GET is
// linearizable by default: a follower always rejects with kWrongLeader
// (there's nothing to read locally that's guaranteed current), and a
// leader confirms it still holds a live quorum (RaftNode::
// ConfirmLeadershipQuorum, a read_index-style check) before reading —
// itself rejecting with kWrongLeader if that check fails. Pass
// allow_stale_reads=true to skip both checks and read local storage
// unconditionally, on any node, for the old possibly-stale behavior.
class RequestHandler {
 public:
  explicit RequestHandler(persistence::DurableStorage& storage, raft::RaftNode* raft = nullptr,
                           bool allow_stale_reads = false);

  protocol::ClientResponse Handle(const protocol::ClientRequest& req);

  // Answers a node-to-node RPC: Ping directly, RequestVote/AppendEntries by
  // decoding into raft and re-encoding its response.
  protocol::ClusterResponse HandleCluster(const protocol::ClusterRequest& req);

 private:
  bool IsLeader() const;

  persistence::DurableStorage& storage_;
  raft::RaftNode* raft_;
  bool allow_stale_reads_;
};

}  // namespace neuralkv
