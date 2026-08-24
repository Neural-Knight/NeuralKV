#pragma once

// Test-only fault injection for Raft's cluster transport (not linked into
// production). Simulates faults in-process by intercepting SendRpc calls,
// not via iptables/network namespaces — enough to test drop, delay, and partition-heal behavior.

#include <chrono>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "cluster/transport.h"
#include "common/result.h"
#include "protocol/types.h"

namespace neuralkv::testing {

// A ClusterTransport that can drop, delay, or probabilistically drop RPCs to
// specific peers on command. Each node gets its own instance controlling only
// its outbound side — a bidirectional partition needs both sides touched (see FaultInjectionController::partition).
class FaultInjectingTransport : public cluster::ClusterTransport {
 public:
  using cluster::ClusterTransport::ClusterTransport;

  // Unconditional drop, on or off, independent of set_drop_rate.
  void set_drop_outbound_to(uint32_t peer_id, bool drop) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (drop) {
      dropped_peers_.insert(peer_id);
    } else {
      dropped_peers_.erase(peer_id);
    }
  }

  // Sleeps ms before forwarding to the real transport. 0 clears any
  // configured delay for this peer.
  void set_delay_ms(uint32_t peer_id, int64_t ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ms <= 0) {
      delay_ms_.erase(peer_id);
    } else {
      delay_ms_[peer_id] = ms;
    }
  }

  // Probabilistic drop, evaluated independently per call. rate is
  // clamped to [0, 1]; 0 clears it.
  void set_drop_rate(uint32_t peer_id, double rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rate <= 0.0) {
      drop_rate_.erase(peer_id);
    } else {
      drop_rate_[peer_id] = rate > 1.0 ? 1.0 : rate;
    }
  }

  // Resets every fault configured on this transport (not other nodes').
  void clear_faults() {
    std::lock_guard<std::mutex> lock(mutex_);
    dropped_peers_.clear();
    delay_ms_.clear();
    drop_rate_.clear();
  }

  Result<protocol::ClusterResponse> SendRpc(const cluster::PeerInfo& peer,
                                             const protocol::ClusterRequest& req) override {
    int64_t delay_ms = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (dropped_peers_.count(peer.node_id) > 0) {
        return Status::Error(ErrorCode::kIOError, "fault injection: outbound RPC dropped");
      }
      const auto rate_it = drop_rate_.find(peer.node_id);
      if (rate_it != drop_rate_.end() && dist_(rng_) < rate_it->second) {
        return Status::Error(ErrorCode::kIOError, "fault injection: outbound RPC dropped (rate)");
      }
      const auto delay_it = delay_ms_.find(peer.node_id);
      if (delay_it != delay_ms_.end()) delay_ms = delay_it->second;
    }
    if (delay_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    return cluster::ClusterTransport::SendRpc(peer, req);
  }

 private:
  std::mutex mutex_;
  std::unordered_set<uint32_t> dropped_peers_;
  std::unordered_map<uint32_t, int64_t> delay_ms_;
  std::unordered_map<uint32_t, double> drop_rate_;
  std::mt19937 rng_{std::random_device{}()};
  std::uniform_real_distribution<double> dist_{0.0, 1.0};
};

// Coordinates fault injection across per-node transports keyed by node id.
// Each transport only controls its own outbound side, so simulating a
// partition between two nodes means touching both — this wraps that into one call.
class FaultInjectionController {
 public:
  void Register(uint32_t node_id, FaultInjectingTransport& transport) {
    transports_[node_id] = &transport;
  }

  // Drops (enabled=true) or restores (enabled=false) RPCs in both
  // directions between node ids a and b. A no-op for either id that
  // was never registered.
  void partition(uint32_t a, uint32_t b, bool enabled) {
    if (FaultInjectingTransport* ta = Find(a)) ta->set_drop_outbound_to(b, enabled);
    if (FaultInjectingTransport* tb = Find(b)) tb->set_drop_outbound_to(a, enabled);
  }

  // Resets every registered transport's injection state.
  void clear_faults() {
    for (auto& [node_id, transport] : transports_) transport->clear_faults();
  }

 private:
  FaultInjectingTransport* Find(uint32_t node_id) {
    const auto it = transports_.find(node_id);
    return it != transports_.end() ? it->second : nullptr;
  }

  std::unordered_map<uint32_t, FaultInjectingTransport*> transports_;
};

}  // namespace neuralkv::testing
