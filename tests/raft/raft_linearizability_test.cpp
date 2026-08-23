#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "cluster/cluster_config.h"
#include "cluster/transport.h"
#include "common/result.h"
#include "net/socket_utils.h"
#include "persistence/durable_storage.h"
#include "protocol/codec.h"
#include "protocol/types.h"
#include "raft/node.h"
#include "server/blocking_server.h"
#include "testing/linearizability_checker.h"

namespace neuralkv::raft {
namespace {

class TempDir {
 public:
  TempDir() {
    char pattern[] = "/tmp/nkv_lin_test_XXXXXX";
    const char* dir = ::mkdtemp(pattern);
    if (dir == nullptr) {
      std::perror("mkdtemp");
      std::abort();
    }
    path_ = dir;
  }

  ~TempDir() {
    ::unlink((path_ + "/wal/wal.log").c_str());
    ::rmdir((path_ + "/wal").c_str());
    ::unlink((path_ + "/raft/state.bin").c_str());
    ::rmdir((path_ + "/raft").c_str());
    ::rmdir(path_.c_str());
  }

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

// Plain (no fault injection) 3-node in-process cluster — same wiring as
// raft_figure8_test.cpp's TestCluster, kept local since this test drives
// real client requests over TCP rather than calling RaftNode directly.
class LinTestCluster {
 public:
  explicit LinTestCluster(std::vector<uint16_t> ports) : ports_(ports) {
    std::vector<cluster::PeerInfo> peers;
    for (std::size_t i = 0; i < ports.size(); ++i) {
      peers.push_back(cluster::PeerInfo{
          .node_id = static_cast<uint32_t>(i + 1), .host = "127.0.0.1", .port = ports[i]});
    }
    for (std::size_t i = 0; i < ports.size(); ++i) {
      auto node = std::make_unique<Node>();
      node->config.local_node_id = static_cast<uint32_t>(i + 1);
      node->config.peers = peers;
      StartNode(*node, ports[i]);
      nodes_.push_back(std::move(node));
    }
  }

  ~LinTestCluster() {
    for (auto& node : nodes_) {
      node->raft->Stop();
      node->server->Stop();
      if (node->server_thread.joinable()) node->server_thread.join();
    }
  }

  uint16_t PortOf(uint32_t node_id) const { return ports_[node_id - 1]; }

  int WaitForLeader(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      int leader_index = -1;
      int leader_count = 0;
      for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i]->raft->state() == RaftState::kLeader) {
          leader_index = static_cast<int>(i);
          ++leader_count;
        }
      }
      if (leader_count == 1) return leader_index;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return -1;
  }

 private:
  struct Node {
    cluster::ClusterConfig config;
    TempDir data_dir;
    std::unique_ptr<persistence::DurableStorage> storage;
    std::unique_ptr<cluster::ClusterTransport> transport;
    std::unique_ptr<RaftNode> raft;
    std::unique_ptr<BlockingServer> server;
    std::thread server_thread;
  };

  static void StartNode(Node& node, uint16_t port) {
    node.storage = std::make_unique<persistence::DurableStorage>(node.data_dir.path());
    node.transport = std::make_unique<cluster::ClusterTransport>(node.config.local_node_id);
    node.raft = std::make_unique<RaftNode>(node.config.local_node_id, node.config, *node.storage,
                                            *node.transport);
    ASSERT_TRUE(node.raft->open_status().ok()) << node.raft->open_status().message();
    node.server = std::make_unique<BlockingServer>("127.0.0.1", port, *node.storage, node.raft.get());
    node.raft->Start();
    node.server_thread = std::thread([s = node.server.get()] { s->Run(); });
  }

  std::vector<uint16_t> ports_;
  std::vector<std::unique_ptr<Node>> nodes_;
};

// One request/response round trip against host:port, or a status error
// if the connection itself fails.
Result<protocol::ClientResponse> SendClientRequest(uint16_t port, const protocol::ClientRequest& req) {
  Result<int> conn = net::TcpConnect("127.0.0.1", port);
  if (!conn.ok()) return conn.status();
  net::Fd fd(conn.value());

  std::vector<uint8_t> encoded;
  Status status = protocol::EncodeClientRequest(req, encoded);
  if (!status.ok()) return status;
  status = net::WriteFull(fd.get(), encoded.data(), encoded.size());
  if (!status.ok()) return status;

  std::vector<uint8_t> buffer;
  uint8_t chunk[4096];
  protocol::ClientResponse resp;
  while (true) {
    const protocol::ParseResult result = protocol::TryParseFrame(buffer, nullptr, &resp);
    if (result == protocol::ParseResult::kComplete) return resp;
    if (result == protocol::ParseResult::kError) {
      return Status::Error(ErrorCode::kIOError, "malformed response from server");
    }
    const ssize_t n = ::read(fd.get(), chunk, sizeof(chunk));
    if (n <= 0) return Status::Error(ErrorCode::kIOError, "connection closed before response received");
    buffer.insert(buffer.end(), chunk, chunk + n);
  }
}

// A real client: sends to whichever port it currently believes is
// leader, follows exactly one WRONG_LEADER redirect (same policy as
// nkv-client), and remembers the redirect target for its next op.
class LinClient {
 public:
  LinClient(LinTestCluster& cluster, uint16_t initial_port) : cluster_(cluster), port_(initial_port) {}

  Result<protocol::ClientResponse> Send(const protocol::ClientRequest& req) {
    Result<protocol::ClientResponse> resp = SendClientRequest(port_, req);
    if (resp.ok() && resp.value().status == protocol::ResponseStatus::kWrongLeader &&
        resp.value().leader_hint != 0) {
      port_ = cluster_.PortOf(resp.value().leader_hint);
      resp = SendClientRequest(port_, req);
    }
    return resp;
  }

 private:
  LinTestCluster& cluster_;
  uint16_t port_;
};

TEST(RaftLinearizabilityTest, ThreeClientsSingleKeyFiveHundredOps) {
  const std::vector<uint16_t> ports = {18791, 18792, 18793};
  LinTestCluster cluster(ports);
  const int leader = cluster.WaitForLeader(std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0);

  constexpr const char* kKey = "shared-key";
  constexpr int kClientCount = 3;
  const std::array<int, kClientCount> ops_per_client = {167, 167, 166};  // sums to 500

  testing::LinearizabilityChecker checker;
  std::mutex checker_mutex;
  std::atomic<uint64_t> clock{0};

  std::vector<std::thread> clients;
  for (int c = 0; c < kClientCount; ++c) {
    clients.emplace_back([&, c] {
      LinClient client(cluster, ports[static_cast<std::size_t>(c) % ports.size()]);
      std::mt19937 rng(1000 + static_cast<unsigned>(c));
      std::uniform_int_distribution<int> coin(0, 1);

      for (int i = 0; i < ops_per_client[static_cast<std::size_t>(c)]; ++i) {
        const bool do_set = coin(rng) == 0;
        protocol::ClientRequest req;
        req.request_id = static_cast<uint64_t>(i);
        std::string written_value;
        if (do_set) {
          written_value = "c" + std::to_string(c) + "-" + std::to_string(i);
          req.opcode = protocol::Opcode::kSet;
          req.key = kKey;
          req.value = written_value;
        } else {
          req.opcode = protocol::Opcode::kGet;
          req.key = kKey;
        }

        const uint64_t start = clock.fetch_add(1);
        const Result<protocol::ClientResponse> resp = client.Send(req);
        const uint64_t end = clock.fetch_add(1);
        if (!resp.ok()) continue;  // connection-level failure; nothing to check

        const bool succeeded = resp.value().status == protocol::ResponseStatus::kOk;
        if (do_set) {
          if (!succeeded) continue;
          std::lock_guard<std::mutex> lock(checker_mutex);
          checker.Record(testing::LinearizabilityOp{
              c, testing::LinearizabilityOp::Kind::kSet, written_value, true, start, end});
        } else {
          // A GET before the key's first SET legitimately returns
          // NotFound — not a value, nothing to check against.
          if (!succeeded) continue;
          std::lock_guard<std::mutex> lock(checker_mutex);
          checker.Record(testing::LinearizabilityOp{
              c, testing::LinearizabilityOp::Kind::kGet, resp.value().value, true, start, end});
        }
      }
    });
  }
  for (std::thread& t : clients) t.join();

  const std::string violation = checker.Check();
  EXPECT_TRUE(violation.empty()) << violation;
}

}  // namespace
}  // namespace neuralkv::raft
