#pragma once

#include "persistence/durable_storage.h"
#include "protocol/types.h"
#include "raft/node.h"

namespace neuralkv {

// Applies decoded client requests to storage and answers cluster RPCs. Without a
// RaftNode every write/read is local; with one, only the leader accepts SET/DELETE
// and GET is linearizable by default (quorum-confirmed) unless allow_stale_reads=true.
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
