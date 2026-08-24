#include "testing/fault_injection.h"

#include <gtest/gtest.h>

#include <chrono>

#include "cluster/cluster_config.h"

namespace neuralkv::testing {
namespace {

// No real server needs to be listening: fault-injection gating (drop, delay,
// partition, clear) short-circuits before any connection attempt matters. Where
// it does fall through, the connect fails fast and is checked only for the fault-injection error text.
cluster::PeerInfo PeerAt(uint32_t node_id) {
  return cluster::PeerInfo{.node_id = node_id, .host = "127.0.0.1", .port = 18999};
}

protocol::ClusterRequest Ping() {
  return protocol::ClusterRequest{.request_id = 1, .opcode = protocol::ClusterOpcode::kPing, .body = ""};
}

TEST(FaultInjectionTest, DropOutboundBlocksTheCall) {
  FaultInjectingTransport transport(1);
  transport.set_drop_outbound_to(2, true);

  const Result<protocol::ClusterResponse> result = transport.SendRpc(PeerAt(2), Ping());
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("fault injection"), std::string::npos);
}

TEST(FaultInjectionTest, DropOutboundOffAllowsFallThrough) {
  FaultInjectingTransport transport(1);
  transport.set_drop_outbound_to(2, true);
  transport.set_drop_outbound_to(2, false);

  const Result<protocol::ClusterResponse> result = transport.SendRpc(PeerAt(2), Ping());
  ASSERT_FALSE(result.ok());  // nothing is listening on the probe port
  EXPECT_EQ(result.status().message().find("fault injection"), std::string::npos);
}

TEST(FaultInjectionTest, DropRateOneAlwaysDrops) {
  FaultInjectingTransport transport(1);
  transport.set_drop_rate(2, 1.0);

  for (int i = 0; i < 5; ++i) {
    const Result<protocol::ClusterResponse> result = transport.SendRpc(PeerAt(2), Ping());
    ASSERT_FALSE(result.ok());
    EXPECT_NE(result.status().message().find("fault injection"), std::string::npos);
  }
}

TEST(FaultInjectionTest, DropRateZeroNeverAppliesRateDrop) {
  FaultInjectingTransport transport(1);
  transport.set_drop_rate(2, 0.0);  // clears/no-ops

  const Result<protocol::ClusterResponse> result = transport.SendRpc(PeerAt(2), Ping());
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.status().message().find("fault injection"), std::string::npos);
}

TEST(FaultInjectionTest, DelayAddsAtLeastTheConfiguredLatency) {
  FaultInjectingTransport transport(1);
  transport.set_delay_ms(2, 50);

  const auto start = std::chrono::steady_clock::now();
  transport.SendRpc(PeerAt(2), Ping());
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_GE(elapsed, std::chrono::milliseconds(50));
}

TEST(FaultInjectionTest, ClearFaultsResetsDropAndDelay) {
  FaultInjectingTransport transport(1);
  transport.set_drop_outbound_to(2, true);
  transport.set_delay_ms(2, 5000);
  transport.set_drop_rate(2, 1.0);

  transport.clear_faults();

  const auto start = std::chrono::steady_clock::now();
  const Result<protocol::ClusterResponse> result = transport.SendRpc(PeerAt(2), Ping());
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(result.status().message().find("fault injection"), std::string::npos);
  EXPECT_LT(elapsed, std::chrono::milliseconds(1000)) << "delay should have been cleared";
}

TEST(FaultInjectionTest, ControllerPartitionDropsBothDirections) {
  FaultInjectingTransport node1(1);
  FaultInjectingTransport node2(2);
  FaultInjectionController controller;
  controller.Register(1, node1);
  controller.Register(2, node2);

  controller.partition(1, 2, true);

  EXPECT_FALSE(node1.SendRpc(PeerAt(2), Ping()).ok());
  EXPECT_NE(node1.SendRpc(PeerAt(2), Ping()).status().message().find("fault injection"), std::string::npos);
  EXPECT_FALSE(node2.SendRpc(PeerAt(1), Ping()).ok());
  EXPECT_NE(node2.SendRpc(PeerAt(1), Ping()).status().message().find("fault injection"), std::string::npos);

  controller.partition(1, 2, false);

  EXPECT_EQ(node1.SendRpc(PeerAt(2), Ping()).status().message().find("fault injection"), std::string::npos);
  EXPECT_EQ(node2.SendRpc(PeerAt(1), Ping()).status().message().find("fault injection"), std::string::npos);
}

TEST(FaultInjectionTest, ControllerClearFaultsResetsAllRegistered) {
  FaultInjectingTransport node1(1);
  FaultInjectingTransport node2(2);
  FaultInjectionController controller;
  controller.Register(1, node1);
  controller.Register(2, node2);

  controller.partition(1, 2, true);
  controller.clear_faults();

  EXPECT_EQ(node1.SendRpc(PeerAt(2), Ping()).status().message().find("fault injection"), std::string::npos);
  EXPECT_EQ(node2.SendRpc(PeerAt(1), Ping()).status().message().find("fault injection"), std::string::npos);
}

TEST(FaultInjectionTest, PartitionIgnoresUnregisteredNodeIds) {
  FaultInjectingTransport node1(1);
  FaultInjectionController controller;
  controller.Register(1, node1);

  controller.partition(1, 99, true);  // 99 was never registered

  EXPECT_FALSE(node1.SendRpc(PeerAt(99), Ping()).ok());
  EXPECT_NE(node1.SendRpc(PeerAt(99), Ping()).status().message().find("fault injection"), std::string::npos);
}

}  // namespace
}  // namespace neuralkv::testing
