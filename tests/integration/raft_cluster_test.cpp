#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
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

// Fixed ports: every node's cluster config has to name all three
// addresses before any of them start, so ephemeral (--port 0) isn't an
// option here — same constraint as cluster_transport_test.cpp.
constexpr uint16_t kPorts[3] = {18701, 18702, 18703};

std::string WriteClusterConfig(const std::string& dir, uint32_t node_id) {
  const std::string path = dir + "/cluster-" + std::to_string(node_id) + ".conf";
  std::ofstream out(path, std::ios::trunc);
  // No leader_id: this cluster is entirely Raft-elected.
  out << "node_id=" << node_id << "\n";
  for (std::size_t i = 0; i < 3; ++i) {
    out << "peer " << (i + 1) << " 127.0.0.1 " << kPorts[i] << "\n";
  }
  return path;
}

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

// One request/response round trip against host:port, or a status error if
// the connection itself fails (e.g. the node just got SIGKILL'd).
Result<protocol::ClientResponse> TrySendClientRequest(uint16_t port,
                                                        const protocol::ClientRequest& req) {
  Result<int> conn = net::TcpConnect("127.0.0.1", port);
  if (!conn.ok()) return conn.status();
  net::Fd fd(conn.value());
  return SendClientRequest(fd.get(), req);
}

struct ClientResult {
  int exit_code = -1;
  std::string stdout_output;
};

ClientResult RunClient(const std::vector<std::string>& args) {
  int pipe_fds[2];
  if (::pipe(pipe_fds) != 0) {
    std::perror("pipe");
    std::abort();
  }

  const pid_t pid = ::fork();
  if (pid == 0) {
    ::close(pipe_fds[0]);
    ::dup2(pipe_fds[1], STDOUT_FILENO);
    ::close(pipe_fds[1]);

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(NKV_CLIENT_PATH));
    for (const std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    ::execv(NKV_CLIENT_PATH, argv.data());
    std::perror("execv");
    ::_exit(127);
  }

  ::close(pipe_fds[1]);
  std::string output;
  char buf[256];
  ssize_t n;
  while ((n = ::read(pipe_fds[0], buf, sizeof(buf))) > 0) {
    output.append(buf, static_cast<std::size_t>(n));
  }
  ::close(pipe_fds[0]);

  int status = 0;
  ::waitpid(pid, &status, 0);
  ClientResult result;
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  result.stdout_output = std::move(output);
  return result;
}

// Forks nkv-server as one Raft cluster node on a fixed port.
class RaftNodeProcess {
 public:
  RaftNodeProcess(uint32_t node_id, uint16_t port, const std::string& cluster_conf_path) {
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

  ~RaftNodeProcess() { Kill(SIGTERM); }

  uint16_t port() const { return port_; }
  bool alive() const { return pid_ > 0; }

  void Kill(int sig) {
    if (pid_ <= 0) return;
    ::kill(pid_, sig);
    int status = 0;
    ::waitpid(pid_, &status, 0);
    pid_ = -1;
    if (read_fd_ >= 0) {
      ::close(read_fd_);
      read_fd_ = -1;
    }
  }

 private:
  testutil::TempDataDir data_dir_;
  pid_t pid_ = -1;
  int read_fd_ = -1;
  uint16_t port_ = 0;
};

class RaftClusterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    for (uint32_t id = 1; id <= 3; ++id) {
      conf_paths_.push_back(WriteClusterConfig(config_dir_.path(), id));
    }
    for (uint32_t id = 1; id <= 3; ++id) {
      nodes_.push_back(std::make_unique<RaftNodeProcess>(id, kPorts[id - 1], conf_paths_[id - 1]));
      ASSERT_NE(nodes_.back()->port(), 0);
    }
  }

  void TearDown() override {
    nodes_.clear();
    for (const std::string& path : conf_paths_) ::unlink(path.c_str());
  }

  // Repeatedly probes every live node with a SET until one accepts it
  // (i.e. is the current leader), or the deadline passes. The generous
  // deadline accounts for this test's own fork/exec/connect overhead on
  // top of Raft's sub-second election timeout.
  int FindLeaderAndSet(const std::string& key, const std::string& value,
                        std::chrono::milliseconds deadline) {
    const auto until = std::chrono::steady_clock::now() + deadline;
    while (std::chrono::steady_clock::now() < until) {
      for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (!nodes_[i]->alive()) continue;
        Result<protocol::ClientResponse> resp = TrySendClientRequest(
            nodes_[i]->port(), protocol::ClientRequest{
                                    .request_id = 1, .opcode = protocol::Opcode::kSet, .key = key, .value = value});
        if (resp.ok() && resp.value().status == protocol::ResponseStatus::kOk) {
          return static_cast<int>(i);
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return -1;
  }

  testutil::TempDataDir config_dir_;
  std::vector<std::string> conf_paths_;
  std::vector<std::unique_ptr<RaftNodeProcess>> nodes_;
};

TEST_F(RaftClusterTest, LeaderWriteReplicatesToFollowers) {
  const int leader = FindLeaderAndSet("k1", "v1", std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0) << "no leader accepted a write";

  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    if (static_cast<int>(i) == leader) continue;

    std::string value;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    bool caught_up = false;
    while (std::chrono::steady_clock::now() < deadline) {
      Result<protocol::ClientResponse> resp = TrySendClientRequest(
          nodes_[i]->port(),
          protocol::ClientRequest{.request_id = 2, .opcode = protocol::Opcode::kGet, .key = "k1"});
      if (resp.ok() && resp.value().status == protocol::ResponseStatus::kOk &&
          resp.value().value == "v1") {
        caught_up = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(caught_up) << "follower " << i << " never caught up to the leader's write";
  }
}

TEST_F(RaftClusterTest, FollowerRedirectsClientToLeader) {
  const int leader = FindLeaderAndSet("seed", "value", std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0);

  const int follower = (leader + 1) % static_cast<int>(nodes_.size());
  Result<protocol::ClientResponse> direct = TrySendClientRequest(
      nodes_[static_cast<std::size_t>(follower)]->port(),
      protocol::ClientRequest{.request_id = 3, .opcode = protocol::Opcode::kSet, .key = "k2", .value = "v2"});
  ASSERT_TRUE(direct.ok());
  EXPECT_EQ(direct.value().status, protocol::ResponseStatus::kWrongLeader);
  EXPECT_NE(direct.value().leader_hint, 0u);

  const ClientResult redirected =
      RunClient({"--port", std::to_string(nodes_[static_cast<std::size_t>(follower)]->port()),
                 "--cluster-config", conf_paths_[static_cast<std::size_t>(follower)], "set", "k2", "v2"});
  EXPECT_EQ(redirected.exit_code, 0);
  EXPECT_NE(redirected.stdout_output.find("OK"), std::string::npos);
}

TEST_F(RaftClusterTest, NewLeaderElectedAfterLeaderCrash) {
  const int leader = FindLeaderAndSet("before-crash", "still-here", std::chrono::milliseconds(2000));
  ASSERT_GE(leader, 0);

  nodes_[static_cast<std::size_t>(leader)]->Kill(SIGKILL);

  // Raft's own election timeout is well under a second (see raft/node.cpp);
  // this budget adds slack for the test harness's fork/exec/connect cost.
  const int new_leader = FindLeaderAndSet("after-crash", "elected", std::chrono::milliseconds(2500));
  ASSERT_GE(new_leader, 0) << "no new leader elected after the old leader was killed";
  EXPECT_NE(new_leader, leader);
}

}  // namespace
}  // namespace neuralkv
