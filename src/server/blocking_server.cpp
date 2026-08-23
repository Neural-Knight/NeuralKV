#include "server/blocking_server.h"

#include <cerrno>
#include <cstring>

#include <sys/socket.h>

#include "common/result.h"
#include "server/connection_session.h"

namespace neuralkv {

BlockingServer::BlockingServer(std::string host, uint16_t port, persistence::DurableStorage& storage,
                                raft::RaftNode* raft)
    : host_(std::move(host)), port_(port), handler_(storage, raft) {
  Result<int> listen_result = net::TcpListen(host_, port_);
  if (!listen_result.ok()) {
    bind_status_ = listen_result.status();
    return;
  }
  listen_fd_ = net::Fd(listen_result.value());
  port_ = net::GetBoundPort(listen_fd_.get(), port_);
}

void BlockingServer::Stop() {
  stop_.store(true);
  // Closing the listen socket reliably wakes a thread blocked in accept()
  // on macOS, but not on Linux — a thread can stay parked in accept() on
  // an fd that's already been closed. Connecting to our own listener
  // forces accept() to return with a real (if unused) connection, which
  // Run()'s loop discards once it sees stop_ set.
  Result<int> wake_conn = net::TcpConnect(host_, port_);
  if (wake_conn.ok()) {
    net::CloseQuietly(wake_conn.value());
  }
  listen_fd_.reset();
}

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
    if (stop_.load()) {
      net::CloseQuietly(client_fd);  // the wake-up connection from Stop(), not a real client
      break;
    }
    ServeConnection(client_fd);
    net::CloseQuietly(client_fd);
  }
  return Status::Ok();
}

void BlockingServer::ServeConnection(int fd) { ServeClientSession(fd, handler_); }

}  // namespace neuralkv
