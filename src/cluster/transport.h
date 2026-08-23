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
//
// SendRpc dials a fresh connection per call and closes it immediately
// after reading the response — deliberately not caching/reusing one per
// peer. Raft's heartbeats are continuous (every ~75ms as long as a node is
// leader); a cached, held-open connection would permanently occupy the
// receiving end's one serving slot under BlockingServer's one-connection-
// at-a-time model, starving every client request to that node. A fresh
// connection per RPC costs a TCP handshake each time but keeps every
// node's server free between calls, and lets independent RPCs to
// different peers run fully concurrently instead of serializing behind a
// shared cache lock.
//
// GetOrConnect/CloseAll are kept for a caller that wants to manage a
// connection's lifetime itself across multiple calls; SendRpc doesn't use
// either.
class ClusterTransport {
 public:
  explicit ClusterTransport(uint32_t local_node_id);
  virtual ~ClusterTransport();

  ClusterTransport(const ClusterTransport&) = delete;
  ClusterTransport& operator=(const ClusterTransport&) = delete;

  // Returns the cached connection to peer, or dials a new one.
  Result<int> GetOrConnect(const PeerInfo& peer);

  // Dials a fresh connection to peer, sends req, blocks for its response,
  // and closes the connection before returning — success or failure.
  // Virtual solely so tests can wrap it with fault injection (see
  // src/testing/fault_injection.h); RaftNode always talks to a plain
  // ClusterTransport in production.
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
