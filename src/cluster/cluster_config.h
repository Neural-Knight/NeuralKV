#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/status.h"

namespace neuralkv::cluster {

struct PeerInfo {
  uint32_t node_id = 0;
  std::string host;
  uint16_t port = 0;
};

struct ClusterConfig {
  uint32_t local_node_id = 0;
  uint32_t leader_node_id = 0;  // static leader until Raft (M8)
  std::vector<PeerInfo> peers;

  const PeerInfo* FindPeer(uint32_t node_id) const;
};

// Parses a cluster config file:
//
//   node_id=1
//   leader_id=1
//   peer 1 127.0.0.1 7401
//   peer 2 127.0.0.1 7402
//   peer 3 127.0.0.1 7403
//
// Blank lines and lines starting with '#' are ignored. peers must list
// every node in the cluster, including the local one. Validates that
// local_node_id and leader_id both name a peer in the list and that no
// peer id repeats.
Status LoadClusterConfig(const std::string& path, ClusterConfig& out);

}  // namespace neuralkv::cluster
