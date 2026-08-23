#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "cluster/cluster_config.h"
#include "common/result.h"
#include "protocol/types.h"

namespace neuralkv::cluster {

// Node-to-node RPC over the same framed TCP protocol client connections
// use, distinguished by MessageType (kClusterRequest/kClusterResponse).
// Connections are cached per peer and reused across calls. One RPC at a
// time across the whole transport — fine for the low-volume liveness
// checks this stage needs; a future stage can shard the lock per peer if
// concurrent RPC volume grows.
class ClusterTransport {
 public:
  explicit ClusterTransport(uint32_t local_node_id);
  ~ClusterTransport();

  ClusterTransport(const ClusterTransport&) = delete;
  ClusterTransport& operator=(const ClusterTransport&) = delete;

  // Returns the cached connection to peer, or dials a new one.
  Result<int> GetOrConnect(const PeerInfo& peer);

  // Sends req to peer and blocks for its response. A malformed response
  // or I/O failure closes and discards the cached connection so the next
  // call reconnects instead of reusing a socket left in a bad state.
  Result<protocol::ClusterResponse> SendRpc(const PeerInfo& peer,
                                             const protocol::ClusterRequest& req);

  void CloseAll();

 private:
  // Not yet read anywhere: no outgoing RPC carries a sender identity until
  // Raft needs one. Kept so the constructor signature doesn't have to
  // change when that lands.
  [[maybe_unused]] uint32_t local_node_id_;
  std::unordered_map<uint32_t, int> peer_fds_;  // node_id -> connected fd
  std::mutex mutex_;
};

}  // namespace neuralkv::cluster
