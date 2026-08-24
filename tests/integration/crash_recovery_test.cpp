#include <gtest/gtest.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
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

using testutil::TempDataDir;

// Forks nkv-server against a given --data-dir and an ephemeral port,
// parsing the port it reports on stdout. Unlike the other integration
// tests' TestServer, this one exposes Crash() so a test can SIGKILL the
// server mid-test and later fork a fresh instance over the same data_dir.
class ManagedServer {
 public:
  explicit ManagedServer(const std::string& data_dir) {
    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) {
      std::perror("pipe");
      std::abort();
    }

    pid_ = ::fork();
    if (pid_ == 0) {
      ::close(pipe_fds[0]);
      ::dup2(pipe_fds[1], STDOUT_FILENO);
      ::dup2(pipe_fds[1], STDERR_FILENO);
      ::close(pipe_fds[1]);
      ::execl(NKV_SERVER_PATH, NKV_SERVER_PATH, "--port", "0", "--data-dir", data_dir.c_str(),
              nullptr);
      std::perror("execl");
      ::_exit(127);
    }

    ::close(pipe_fds[1]);
    read_fd_ = pipe_fds[0];
    port_ = ParsePortFromChildOutput();
  }

  ~ManagedServer() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      int status = 0;
      ::waitpid(pid_, &status, 0);
    }
    if (read_fd_ >= 0) {
      ::close(read_fd_);
    }
  }

  // SIGKILLs the server with no chance to run its shutdown path, simulating
  // a hard crash rather than a graceful stop.
  void Crash() {
    ::kill(pid_, SIGKILL);
    int status = 0;
    ::waitpid(pid_, &status, 0);
    ::close(read_fd_);
    read_fd_ = -1;
    pid_ = -1;
  }

  uint16_t port() const { return port_; }

 private:
  uint16_t ParsePortFromChildOutput() {
    std::string line;
    char c = 0;
    while (::read(read_fd_, &c, 1) == 1 && c != '\n') {
      line.push_back(c);
    }
    const size_t colon = line.rfind(':');
    if (colon == std::string::npos) return 0;
    return static_cast<uint16_t>(std::stoi(line.substr(colon + 1)));
  }

  pid_t pid_ = -1;
  int read_fd_ = -1;
  uint16_t port_ = 0;
};

protocol::ClientResponse SendRequestAndWait(int fd, const protocol::ClientRequest& req) {
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

TEST(CrashRecoveryTest, CommittedWritesSurviveSigkillAndRestart) {
  TempDataDir dir;
  const std::vector<std::pair<std::string, std::string>> kEntries = {
      {"k1", "v1"}, {"k2", "v2"}, {"k3", "v3"}};

  {
    ManagedServer server(dir.path());
    Result<int> conn = net::TcpConnect("127.0.0.1", server.port());
    ASSERT_TRUE(conn.ok());
    net::Fd fd(conn.value());

    uint64_t request_id = 1;
    for (const auto& [key, value] : kEntries) {
      const protocol::ClientResponse resp =
          SendRequestAndWait(fd.get(), protocol::ClientRequest{.request_id = request_id++,
                                                                 .opcode = protocol::Opcode::kSet,
                                                                 .key = key,
                                                                 .value = value});
      ASSERT_EQ(resp.status, protocol::ResponseStatus::kOk);
    }

    server.Crash();
  }

  ManagedServer restarted(dir.path());
  Result<int> conn = net::TcpConnect("127.0.0.1", restarted.port());
  ASSERT_TRUE(conn.ok());
  net::Fd fd(conn.value());

  uint64_t request_id = 1;
  for (const auto& [key, expected] : kEntries) {
    const protocol::ClientResponse resp =
        SendRequestAndWait(fd.get(), protocol::ClientRequest{.request_id = request_id++,
                                                               .opcode = protocol::Opcode::kGet,
                                                               .key = key,
                                                               .value = ""});
    EXPECT_EQ(resp.status, protocol::ResponseStatus::kOk);
    EXPECT_EQ(resp.value, expected);
  }
}

}  // namespace
}  // namespace neuralkv
