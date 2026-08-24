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
  // Bootstrap/legacy leader designation; ignored once Raft elects its own
  // leader. 0 means absent.
  uint32_t leader_node_id = 0;
  std::vector<PeerInfo> peers;

  const PeerInfo* FindPeer(uint32_t node_id) const;
};

// Parses a cluster config file: node_id=N, optional leader_id=N, and one
// "peer <id> <host> <port>" line per node (including local). Blank/'#'
// lines are ignored; local_node_id must appear as a peer with no duplicate ids.
Status LoadClusterConfig(const std::string& path, ClusterConfig& out);

}  // namespace neuralkv::cluster
