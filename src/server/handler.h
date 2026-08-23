#pragma once

#include "cluster/cluster_config.h"
#include "persistence/durable_storage.h"
#include "protocol/types.h"

namespace neuralkv {

// Applies a decoded client request to durable storage and builds the
// response, and answers node-to-node cluster RPCs (liveness ping, for
// now). Owns no state beyond the references passed in.
//
// Without a cluster config, the handler behaves exactly as a single node
// always does: every write applies locally. With one, only the
// configured leader applies SET/DELETE; a follower rejects them with
// kWrongLeader and the leader's node id, without touching storage. GET is
// always served from local storage regardless of leadership — a
// follower's WAL is caught up via a separate replication path once one
// exists, so this is a stale read on a follower for now, not a redirect.
class RequestHandler {
 public:
  explicit RequestHandler(persistence::DurableStorage& storage,
                           const cluster::ClusterConfig* cluster_config = nullptr);

  protocol::ClientResponse Handle(const protocol::ClientRequest& req);

  // Answers a node-to-node RPC.
  protocol::ClusterResponse HandleCluster(const protocol::ClusterRequest& req);

 private:
  bool IsLeader() const;

  persistence::DurableStorage& storage_;
  const cluster::ClusterConfig* cluster_config_;
};

}  // namespace neuralkv
