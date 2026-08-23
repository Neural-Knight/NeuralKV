#pragma once

// Test-only fault injection for Raft's cluster transport. Not linked into
// any production binary — only into test targets that explicitly include
// it. A full fault-injection harness (latency injection, message
// reordering, asymmetric partitions, scripted fault schedules) is out of
// scope here; this is deliberately just enough to test "replication
// resumes once a fault heals."

#include <mutex>
#include <unordered_set>

#include "cluster/transport.h"
#include "common/result.h"
#include "protocol/types.h"

namespace neuralkv::testing {

// A ClusterTransport that can drop outbound RPCs to specific peers on
// command, standing in for RaftNode's real transport in a test. Each node
// under test gets its own instance; simulating a bidirectional partition
// between two nodes means calling set_drop_outbound_to on both nodes'
// instances, once for each direction.
class FaultInjectingTransport : public cluster::ClusterTransport {
 public:
  using cluster::ClusterTransport::ClusterTransport;

  void set_drop_outbound_to(uint32_t peer_id, bool drop) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (drop) {
      dropped_peers_.insert(peer_id);
    } else {
      dropped_peers_.erase(peer_id);
    }
  }

  Result<protocol::ClusterResponse> SendRpc(const cluster::PeerInfo& peer,
                                             const protocol::ClusterRequest& req) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (dropped_peers_.count(peer.node_id) > 0) {
        return Status::Error(ErrorCode::kIOError, "fault injection: outbound RPC dropped");
      }
    }
    return cluster::ClusterTransport::SendRpc(peer, req);
  }

 private:
  std::mutex mutex_;
  std::unordered_set<uint32_t> dropped_peers_;
};

}  // namespace neuralkv::testing
