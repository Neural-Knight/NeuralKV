#pragma once

// Shared subprocess-cluster helpers for the Raft integration tests: fork
// real nkv-server processes on fixed ports with generated cluster config
// files, talk to them with the wire protocol directly, and drive
// nkv-client for redirect tests. Requires NKV_SERVER_PATH and
// NKV_CLIENT_PATH to be defined by the including test binary's build
// target.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <csignal>
#include <fstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "common/result.h"
#include "net/socket_utils.h"
#include "protocol/codec.h"
#include "protocol/types.h"
#include "test_support.h"

namespace neuralkv::testutil {

// Writes a cluster config naming every port in `ports` as a peer, with
// this file's own node_id set to `node_id`. No leader_id: every cluster
// built with this helper is entirely Raft-elected.
inline std::string WriteClusterConfig(const std::string& dir, uint32_t node_id,
                                       const std::vector<uint16_t>& ports) {
  const std::string path = dir + "/cluster-" + std::to_string(node_id) + ".conf";
  std::ofstream out(path, std::ios::trunc);
  out << "node_id=" << node_id << "\n";
  for (std::size_t i = 0; i < ports.size(); ++i) {
    out << "peer " << (i + 1) << " 127.0.0.1 " << ports[i] << "\n";
  }
  return path;
}

inline protocol::ClientResponse SendClientRequest(int fd, const protocol::ClientRequest& req) {
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
inline Result<protocol::ClientResponse> TrySendClientRequest(uint16_t port,
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

inline ClientResult RunClient(const std::vector<std::string>& args) {
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

// Forks nkv-server as one Raft cluster node on a fixed port, against a
// caller-owned data directory (so a test can Kill() and then construct a
// fresh RaftNodeProcess pointed at the same directory to simulate a
// restart with its on-disk state intact). extra_args lets a test add
// flags (e.g. --allow-stale-reads) beyond the standard --port/--node-id/
// --cluster-config/--data-dir set.
class RaftNodeProcess {
 public:
  RaftNodeProcess(uint32_t node_id, uint16_t port, const std::string& cluster_conf_path,
                   const std::string& data_dir, std::vector<std::string> extra_args = {}) {
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

      std::vector<const char*> argv = {NKV_SERVER_PATH,
                                        "--port",
                                        port_arg.c_str(),
                                        "--node-id",
                                        node_id_arg.c_str(),
                                        "--cluster-config",
                                        cluster_conf_path.c_str(),
                                        "--data-dir",
                                        data_dir.c_str()};
      for (const std::string& arg : extra_args) argv.push_back(arg.c_str());
      argv.push_back(nullptr);
      ::execv(NKV_SERVER_PATH, const_cast<char**>(argv.data()));
      std::perror("execv");
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
  pid_t pid_ = -1;
  int read_fd_ = -1;
  uint16_t port_ = 0;
};

}  // namespace neuralkv::testutil
