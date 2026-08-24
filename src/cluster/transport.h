#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "cluster/cluster_config.h"
#include "common/result.h"
#include "protocol/types.h"

namespace neuralkv::cluster {

// Node-to-node RPC over the framed TCP protocol (kClusterRequest/kClusterResponse).
// SendRpc dials a fresh connection per call rather than caching one: a held-open
// connection would occupy BlockingServer's single serving slot under continuous heartbeat traffic, starving client requests.
class ClusterTransport {
 public:
  explicit ClusterTransport(uint32_t local_node_id);
  virtual ~ClusterTransport();

  ClusterTransport(const ClusterTransport&) = delete;
  ClusterTransport& operator=(const ClusterTransport&) = delete;

  // Returns the cached connection to peer, or dials a new one.
  Result<int> GetOrConnect(const PeerInfo& peer);

  // Dials a fresh connection to peer, sends req, blocks for the response, and
  // closes it before returning (success or failure). Virtual only so tests can
  // wrap it with fault injection; production always uses plain ClusterTransport.
  virtual Result<protocol::ClusterResponse> SendRpc(const PeerInfo& peer,
                                                      const protocol::ClusterRequest& req);

  void CloseAll();

 private:
  // Not yet read anywhere: no outgoing RPC carries a sender identity until
  // Raft needs one. Kept so the constructor signature doesn't have to
  // change when that lands.
  [[maybe_unused]] uint32_t local_node_id_;
  std::unordered_map<uint32_t, int> peer_fds_;  // node_id -> connected fd (GetOrConnect/CloseAll only)
  std::mutex mutex_;
};

}  // namespace neuralkv::cluster
