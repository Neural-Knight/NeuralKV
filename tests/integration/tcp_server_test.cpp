#include <gtest/gtest.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

// Forks nkv-server against an isolated --data-dir and an ephemeral port,
// parsing the port it reports on stdout, giving each test its own server
// instance and its own WAL to talk to.
class TestServer {
 public:
  TestServer() {
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
      ::execl(NKV_SERVER_PATH, NKV_SERVER_PATH, "--port", "0", "--data-dir",
              data_dir_.path().c_str(), nullptr);
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
  // Declared first so the temp dir exists before fork()/execl() reference it.
  testutil::TempDataDir data_dir_;
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

class TcpServerTest : public ::testing::Test {
 protected:
  TestServer server_;
};

TEST_F(TcpServerTest, SetThenGetOverTcp) {
  Result<int> conn = net::TcpConnect("127.0.0.1", server_.port());
  ASSERT_TRUE(conn.ok());
  net::Fd fd(conn.value());

  const protocol::ClientResponse set_resp = SendRequestAndWait(
      fd.get(), protocol::ClientRequest{.request_id = 1,
                                         .opcode = protocol::Opcode::kSet,
                                         .key = "k1",
                                         .value = "v1"});
  EXPECT_EQ(set_resp.status, protocol::ResponseStatus::kOk);

  const protocol::ClientResponse get_resp = SendRequestAndWait(
      fd.get(), protocol::ClientRequest{.request_id = 2,
                                         .opcode = protocol::Opcode::kGet,
                                         .key = "k1",
                                         .value = ""});
  EXPECT_EQ(get_resp.status, protocol::ResponseStatus::kOk);
  EXPECT_EQ(get_resp.value, "v1");
}

TEST_F(TcpServerTest, GetMissingKeyReturnsNotFound) {
  Result<int> conn = net::TcpConnect("127.0.0.1", server_.port());
  ASSERT_TRUE(conn.ok());
  net::Fd fd(conn.value());

  const protocol::ClientResponse resp = SendRequestAndWait(
      fd.get(), protocol::ClientRequest{.request_id = 1,
                                         .opcode = protocol::Opcode::kGet,
                                         .key = "missing",
                                         .value = ""});
  EXPECT_EQ(resp.status, protocol::ResponseStatus::kNotFound);
}

TEST_F(TcpServerTest, DeleteThenGetNotFound) {
  Result<int> conn = net::TcpConnect("127.0.0.1", server_.port());
  ASSERT_TRUE(conn.ok());
  net::Fd fd(conn.value());

  const protocol::ClientResponse set_resp = SendRequestAndWait(
      fd.get(), protocol::ClientRequest{.request_id = 1,
                                         .opcode = protocol::Opcode::kSet,
                                         .key = "k2",
                                         .value = "v2"});
  ASSERT_EQ(set_resp.status, protocol::ResponseStatus::kOk);

  const protocol::ClientResponse delete_resp = SendRequestAndWait(
      fd.get(), protocol::ClientRequest{.request_id = 2,
                                         .opcode = protocol::Opcode::kDelete,
                                         .key = "k2",
                                         .value = ""});
  EXPECT_EQ(delete_resp.status, protocol::ResponseStatus::kOk);

  const protocol::ClientResponse get_resp = SendRequestAndWait(
      fd.get(), protocol::ClientRequest{.request_id = 3,
                                         .opcode = protocol::Opcode::kGet,
                                         .key = "k2",
                                         .value = ""});
  EXPECT_EQ(get_resp.status, protocol::ResponseStatus::kNotFound);
}

TEST_F(TcpServerTest, MalformedFrameClosesConnection) {
  Result<int> conn = net::TcpConnect("127.0.0.1", server_.port());
  ASSERT_TRUE(conn.ok());
  net::Fd fd(conn.value());

  // Well-formed header, wrong magic: the server should close without a
  // response rather than wait for a payload that will never come.
  const std::vector<uint8_t> bad_frame = {0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00};
  ASSERT_TRUE(net::WriteFull(fd.get(), bad_frame.data(), bad_frame.size()).ok());

  uint8_t buf[16];
  const ssize_t n = ::read(fd.get(), buf, sizeof(buf));
  EXPECT_EQ(n, 0);
}

}  // namespace
}  // namespace neuralkv
