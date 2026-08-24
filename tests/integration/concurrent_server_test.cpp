#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

// Forks nkv-server against an isolated --data-dir with the given worker
// count and an ephemeral port, parsing the port it reports on stdout.
class TestServer {
 public:
  explicit TestServer(int workers) {
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
      const std::string workers_arg = std::to_string(workers);
      ::execl(NKV_SERVER_PATH, NKV_SERVER_PATH, "--port", "0", "--workers", workers_arg.c_str(),
              "--data-dir", data_dir_.path().c_str(), nullptr);
      std::perror("execl");
      ::_exit(127);
    }

    ::close(pipe_fds[1]);
    read_fd_ = pipe_fds[0];
    port_ = testutil::ParsePortFromChildOutput(read_fd_);
  }

  ~TestServer() {
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

// Non-asserting variant of the request/response round trip: gtest macros
// are not safe to call from worker threads, so callers running inside
// std::thread bodies must check the returned bool themselves.
bool SendRequestAndWait(int fd, const protocol::ClientRequest& req,
                         protocol::ClientResponse& out_resp) {
  std::vector<uint8_t> encoded;
  if (!protocol::EncodeClientRequest(req, encoded).ok()) return false;
  if (!net::WriteFull(fd, encoded.data(), encoded.size()).ok()) return false;

  std::vector<uint8_t> buffer;
  uint8_t chunk[4096];
  while (true) {
    const protocol::ParseResult result = protocol::TryParseFrame(buffer, nullptr, &out_resp);
    if (result == protocol::ParseResult::kComplete) return true;
    if (result == protocol::ParseResult::kError) return false;

    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n <= 0) return false;
    buffer.insert(buffer.end(), chunk, chunk + n);
  }
}

TEST(ConcurrentServerTest, ConcurrentSetGetManyClients) {
  TestServer server(8);
  constexpr int kThreads = 16;
  std::atomic<bool> failure{false};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&server, &failure, t]() {
      Result<int> conn = net::TcpConnect("127.0.0.1", server.port());
      if (!conn.ok()) {
        failure = true;
        return;
      }
      net::Fd fd(conn.value());
      const std::string key = "concurrent:" + std::to_string(t);

      protocol::ClientResponse resp;
      const protocol::ClientRequest set_req{.request_id = 1,
                                             .opcode = protocol::Opcode::kSet,
                                             .key = key,
                                             .value = "v"};
      if (!SendRequestAndWait(fd.get(), set_req, resp) ||
          resp.status != protocol::ResponseStatus::kOk) {
        failure = true;
        return;
      }

      const protocol::ClientRequest get_req{.request_id = 2,
                                             .opcode = protocol::Opcode::kGet,
                                             .key = key,
                                             .value = ""};
      if (!SendRequestAndWait(fd.get(), get_req, resp) ||
          resp.status != protocol::ResponseStatus::kOk || resp.value != "v") {
        failure = true;
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  EXPECT_FALSE(failure.load());
}

TEST(ConcurrentServerTest, ConcurrentMixedOpsNoCorruption) {
  TestServer server(8);
  constexpr int kThreads = 32;
  constexpr int kCycles = 100;
  std::atomic<bool> failure{false};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&server, &failure, t]() {
      Result<int> conn = net::TcpConnect("127.0.0.1", server.port());
      if (!conn.ok()) {
        failure = true;
        return;
      }
      net::Fd fd(conn.value());
      const std::string key = "mixed:" + std::to_string(t);

      for (int i = 0; i < kCycles; ++i) {
        const std::string value = "v" + std::to_string(i);
        protocol::ClientResponse resp;

        const protocol::ClientRequest set_req{.request_id = static_cast<uint64_t>(i) * 2 + 1,
                                               .opcode = protocol::Opcode::kSet,
                                               .key = key,
                                               .value = value};
        if (!SendRequestAndWait(fd.get(), set_req, resp) ||
            resp.status != protocol::ResponseStatus::kOk) {
          failure = true;
          return;
        }

        const protocol::ClientRequest get_req{.request_id = static_cast<uint64_t>(i) * 2 + 2,
                                               .opcode = protocol::Opcode::kGet,
                                               .key = key,
                                               .value = ""};
        if (!SendRequestAndWait(fd.get(), get_req, resp) ||
            resp.status != protocol::ResponseStatus::kOk || resp.value != value) {
          failure = true;
          return;
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  EXPECT_FALSE(failure.load());
}

TEST(ConcurrentServerTest, ServerStartsWithWorkersFlag) {
  TestServer server(4);
  Result<int> conn = net::TcpConnect("127.0.0.1", server.port());
  ASSERT_TRUE(conn.ok());
  net::Fd fd(conn.value());

  const protocol::ClientRequest req{
      .request_id = 1, .opcode = protocol::Opcode::kSet, .key = "smoke", .value = "ok"};
  protocol::ClientResponse resp;
  ASSERT_TRUE(SendRequestAndWait(fd.get(), req, resp));
  EXPECT_EQ(resp.status, protocol::ResponseStatus::kOk);
}

}  // namespace
}  // namespace neuralkv
