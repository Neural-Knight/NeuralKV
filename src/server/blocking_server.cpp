#include "server/blocking_server.h"

#include <cerrno>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/result.h"
#include "protocol/codec.h"

namespace neuralkv {

namespace {

// Updates port from the OS-assigned value after an ephemeral (port 0) bind.
uint16_t ResolveBoundPort(int fd, uint16_t requested_port) {
  struct sockaddr_storage addr {};
  socklen_t addr_len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &addr_len) != 0) {
    return requested_port;
  }
  if (addr.ss_family == AF_INET) {
    return ntohs(reinterpret_cast<struct sockaddr_in*>(&addr)->sin_port);
  }
  if (addr.ss_family == AF_INET6) {
    return ntohs(reinterpret_cast<struct sockaddr_in6*>(&addr)->sin6_port);
  }
  return requested_port;
}

}  // namespace

BlockingServer::BlockingServer(std::string host, uint16_t port, ShardedKV& kv)
    : host_(std::move(host)), port_(port), handler_(kv) {
  Result<int> listen_result = net::TcpListen(host_, port_);
  if (!listen_result.ok()) {
    bind_status_ = listen_result.status();
    return;
  }
  listen_fd_ = net::Fd(listen_result.value());
  port_ = ResolveBoundPort(listen_fd_.get(), port_);
}

void BlockingServer::Stop() { stop_.store(true); }

Status BlockingServer::Run() {
  if (!bind_status_.ok()) {
    return bind_status_;
  }

  while (!stop_.load()) {
    const int client_fd = ::accept(listen_fd_.get(), nullptr, nullptr);
    if (client_fd < 0) {
      if (errno == EINTR) continue;
      if (stop_.load()) break;
      return Status::Error(ErrorCode::kIOError, std::string("accept: ") + std::strerror(errno));
    }
    ServeConnection(client_fd);
    net::CloseQuietly(client_fd);
  }
  return Status::Ok();
}

void BlockingServer::ServeConnection(int fd) {
  std::vector<uint8_t> read_buffer;
  uint8_t chunk[4096];

  while (true) {
    for (;;) {
      protocol::ClientRequest req;
      const protocol::ParseResult result = protocol::TryParseFrame(read_buffer, &req, nullptr);
      if (result == protocol::ParseResult::kNeedMore) break;
      if (result == protocol::ParseResult::kError) return;

      const protocol::ClientResponse resp = handler_.Handle(req);
      std::vector<uint8_t> encoded;
      if (!protocol::EncodeClientResponse(resp, encoded).ok()) return;
      if (!net::WriteFull(fd, encoded.data(), encoded.size()).ok()) return;
    }

    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (n == 0) return;  // peer closed the connection
    read_buffer.insert(read_buffer.end(), chunk, chunk + n);
  }
}

}  // namespace neuralkv
