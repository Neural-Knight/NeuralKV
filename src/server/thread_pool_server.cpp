#include "server/thread_pool_server.h"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "common/result.h"
#include "server/connection_session.h"

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

ThreadPoolServer::ThreadPoolServer(std::string host, uint16_t port, ShardedKV& kv,
                                    std::size_t num_workers)
    : host_(std::move(host)), port_(port), handler_(kv), pool_(num_workers) {
  Result<int> listen_result = net::TcpListen(host_, port_);
  if (!listen_result.ok()) {
    bind_status_ = listen_result.status();
    return;
  }
  listen_fd_ = net::Fd(listen_result.value());
  port_ = ResolveBoundPort(listen_fd_.get(), port_);
}

void ThreadPoolServer::Stop() {
  stop_.store(true);
  // Closing the listen socket wakes a thread blocked in accept(); the
  // destructor's later close on the same (already invalid) fd is a
  // harmless no-op.
  net::CloseQuietly(listen_fd_.get());
}

Status ThreadPoolServer::Run() {
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
    pool_.Submit([this, client_fd]() {
      ServeClientSession(client_fd, handler_);
      net::CloseQuietly(client_fd);
    });
  }
  return Status::Ok();
}

}  // namespace neuralkv
