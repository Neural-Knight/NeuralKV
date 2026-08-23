#include <gtest/gtest.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "common/result.h"
#include "net/socket_utils.h"
#include "protocol/codec.h"
#include "protocol/types.h"
#include "test_support.h"

namespace neuralkv {
namespace {

// Fixed, test-specific ports: a cluster.conf has to name every peer's
// address before any of them start, so — unlike the other integration
// tests — an ephemeral port negotiated after fork isn't an option here.
constexpr uint16_t kNode1Port = 17401;
constexpr uint16_t kNode2Port = 17402;

std::string WriteClusterConfig(const std::string& dir, uint32_t node_id) {
  const std::string path = dir + "/cluster-" + std::to_string(node_id) + ".conf";
  std::ofstream out(path, std::ios::trunc);
  out << "node_id=" << node_id << "\n"
      << "leader_id=1\n"
      << "peer 1 127.0.0.1 " << kNode1Port << "\n"
      << "peer 2 127.0.0.1 " << kNode2Port << "\n";
  return path;
}

// Forks nkv-server as one cluster node on a fixed port with its own
// generated config and data dir.
class ClusterNode {
 public:
  ClusterNode(uint32_t node_id, uint16_t port, const std::string& cluster_conf_path) {
    const std::string node_id_arg = std::to_string(node_id);
    const std::string port_arg = std::to_string(port);

    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) {
      std::perror("pipe");
      std::abort();
    }

    pid_ = ::fork();
    if (pid_ == 0) {
      ::close(pipe_fds[0]);
      ::dup2(pipe_fds[1], STDOUT_FILENO);
      ::close(pipe_fds[1]);
      ::execl(NKV_SERVER_PATH, NKV_SERVER_PATH, "--port", port_arg.c_str(), "--node-id",
              node_id_arg.c_str(), "--cluster-config", cluster_conf_path.c_str(), "--data-dir",
              data_dir_.path().c_str(), nullptr);
      std::perror("execl");
      ::_exit(127);
    }

    ::close(pipe_fds[1]);
    read_fd_ = pipe_fds[0];
    port_ = testutil::ParsePortFromChildOutput(read_fd_);
  }

  ~ClusterNode() {
    if (pid_ > 0) {
      ::kill(pid_, SIGTERM);
      int status = 0;
      ::waitpid(pid_, &status, 0);
    }
    if (read_fd_ >= 0) {
      ::close(read_fd_);
    }
  }

  uint16_t port() const { return port_; }

 private:
  testutil::TempDataDir data_dir_;
  pid_t pid_ = -1;
  int read_fd_ = -1;
  uint16_t port_ = 0;
};

protocol::ClientResponse SendClientRequest(int fd, const protocol::ClientRequest& req) {
  std::vector<uint8_t> encoded;
  EXPECT_TRUE(protocol::EncodeClientRequest(req, encoded).ok());
  EXPECT_TRUE(net::WriteFull(fd, encoded.data(), encoded.size()).ok());

  std::vector<uint8_t> buffer;
  uint8_t chunk[4096];
  protocol::ClientResponse resp;
  while (true) {
    const protocol::ParseResult result = protocol::TryParseFrame(buffer, nullptr, &resp);
    if (result == protocol::ParseResult::kComplete) return resp;
    if (result == protocol::ParseResult::kError) {
      ADD_FAILURE() << "malformed response from server";
      return resp;
    }
    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n <= 0) {
      ADD_FAILURE() << "connection closed before response received";
      return resp;
    }
    buffer.insert(buffer.end(), chunk, chunk + n);
  }
}

class ClusterTransportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    node1_conf_ = WriteClusterConfig(config_dir_.path(), 1);
    node2_conf_ = WriteClusterConfig(config_dir_.path(), 2);
    node1_ = std::make_unique<ClusterNode>(1, kNode1Port, node1_conf_);
    node2_ = std::make_unique<ClusterNode>(2, kNode2Port, node2_conf_);
    ASSERT_NE(node1_->port(), 0);
    ASSERT_NE(node2_->port(), 0);
  }

  void TearDown() override {
    node1_.reset();
    node2_.reset();
    ::unlink(node1_conf_.c_str());
    ::unlink(node2_conf_.c_str());
  }

  testutil::TempDataDir config_dir_;
  std::string node1_conf_;
  std::string node2_conf_;
  std::unique_ptr<ClusterNode> node1_;
  std::unique_ptr<ClusterNode> node2_;
};

TEST_F(ClusterTransportTest, LeaderAcceptsSet) {
  Result<int> conn = net::TcpConnect("127.0.0.1", node1_->port());
  ASSERT_TRUE(conn.ok());
  net::Fd fd(conn.value());

  const protocol::ClientResponse resp = SendClientRequest(
      fd.get(), protocol::ClientRequest{.request_id = 1,
                                         .opcode = protocol::Opcode::kSet,
                                         .key = "k1",
                                         .value = "v1"});
  EXPECT_EQ(resp.status, protocol::ResponseStatus::kOk);
}

TEST_F(ClusterTransportTest, FollowerRejectsSetWithLeaderHint) {
  Result<int> conn = net::TcpConnect("127.0.0.1", node2_->port());
  ASSERT_TRUE(conn.ok());
  net::Fd fd(conn.value());

  const protocol::ClientResponse resp = SendClientRequest(
      fd.get(), protocol::ClientRequest{.request_id = 1,
                                         .opcode = protocol::Opcode::kSet,
                                         .key = "k1",
                                         .value = "v1"});
  EXPECT_EQ(resp.status, protocol::ResponseStatus::kWrongLeader);
  EXPECT_EQ(resp.leader_hint, 1u);
}

TEST_F(ClusterTransportTest, ClusterPingGetsPongOnSamePort) {
  Result<int> conn = net::TcpConnect("127.0.0.1", node2_->port());
  ASSERT_TRUE(conn.ok());
  net::Fd fd(conn.value());

  std::vector<uint8_t> encoded;
  ASSERT_TRUE(protocol::EncodeClusterRequest(
                  protocol::ClusterRequest{.request_id = 42,
                                            .opcode = protocol::ClusterOpcode::kPing,
                                            .body = ""},
                  encoded)
                  .ok());
  ASSERT_TRUE(net::WriteFull(fd.get(), encoded.data(), encoded.size()).ok());

  std::vector<uint8_t> buffer;
  uint8_t chunk[4096];
  protocol::ClusterResponse resp;
  protocol::MessageType type;
  while (true) {
    const protocol::ParseResult result =
        protocol::TryParseFrame(buffer, nullptr, nullptr, nullptr, &resp, &type);
    if (result == protocol::ParseResult::kComplete) break;
    ASSERT_NE(result, protocol::ParseResult::kError);
    const ssize_t n = ::read(fd.get(), chunk, sizeof(chunk));
    ASSERT_GT(n, 0);
    buffer.insert(buffer.end(), chunk, chunk + n);
  }

  EXPECT_EQ(type, protocol::MessageType::kClusterResponse);
  EXPECT_EQ(resp.request_id, 42u);
  EXPECT_EQ(resp.status, protocol::ResponseStatus::kOk);
  EXPECT_EQ(resp.body, "pong");
}

}  // namespace
}  // namespace neuralkv
