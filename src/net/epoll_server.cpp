#include "net/epoll_server.h"

#ifdef NEURALKV_LINUX

#include <cerrno>
#include <cstring>

#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace neuralkv::net {

namespace {

constexpr int kMaxEvents = 64;
// Once a connection's queued output crosses this, its reads are paused
// (EPOLLIN dropped) until the write buffer drains, so a slow client can't
// make the server buffer unbounded response data in its favor.
constexpr std::size_t kBackpressureThreshold = 1024 * 1024;

uint32_t WantedEvents(const Connection& conn) {
  uint32_t events = EPOLLET | EPOLLRDHUP;
  if (conn.write_buffer_size() < kBackpressureThreshold) {
    events |= EPOLLIN;
  }
  if (conn.write_pending()) {
    events |= EPOLLOUT;
  }
  return events;
}

}  // namespace

EpollServer::EpollServer(std::string host, uint16_t port, ShardedKV& kv)
    : host_(std::move(host)), port_(port), handler_(kv) {
  Result<int> listen_result = TcpListen(host_, port_);
  if (!listen_result.ok()) {
    bind_status_ = listen_result.status();
    return;
  }
  listen_fd_ = Fd(listen_result.value());
  port_ = GetBoundPort(listen_fd_.get(), port_);
  SetNonBlocking(listen_fd_.get(), true);

  const int epoll_raw = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_raw < 0) {
    bind_status_ =
        Status::Error(ErrorCode::kIOError, std::string("epoll_create1: ") + std::strerror(errno));
    return;
  }
  epoll_fd_ = Fd(epoll_raw);

  const int wake_raw = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_raw < 0) {
    bind_status_ =
        Status::Error(ErrorCode::kIOError, std::string("eventfd: ") + std::strerror(errno));
    return;
  }
  wake_fd_ = Fd(wake_raw);

  epoll_event listen_ev{};
  listen_ev.events = EPOLLIN;
  listen_ev.data.fd = listen_fd_.get();
  if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, listen_fd_.get(), &listen_ev) != 0) {
    bind_status_ = Status::Error(ErrorCode::kIOError,
                                  std::string("epoll_ctl(listen): ") + std::strerror(errno));
    return;
  }

  epoll_event wake_ev{};
  wake_ev.events = EPOLLIN;
  wake_ev.data.fd = wake_fd_.get();
  if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, wake_fd_.get(), &wake_ev) != 0) {
    bind_status_ = Status::Error(ErrorCode::kIOError,
                                  std::string("epoll_ctl(wake): ") + std::strerror(errno));
    return;
  }
}

void EpollServer::Stop() {
  stop_.store(true);
  const uint64_t one = 1;
  // Best-effort: if this write fails, the loop still notices stop_ the
  // next time any other event wakes epoll_wait.
  const ssize_t wake_result = ::write(wake_fd_.get(), &one, sizeof(one));
  (void)wake_result;
}

Status EpollServer::Run() {
  if (!bind_status_.ok()) {
    return bind_status_;
  }

  epoll_event events[kMaxEvents];
  while (!stop_.load()) {
    const int n = ::epoll_wait(epoll_fd_.get(), events, kMaxEvents, -1);
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::Error(ErrorCode::kIOError, std::string("epoll_wait: ") + std::strerror(errno));
    }

    for (int i = 0; i < n; ++i) {
      const int fd = events[i].data.fd;
      if (fd == wake_fd_.get()) {
        uint64_t value = 0;
        const ssize_t drain_result = ::read(wake_fd_.get(), &value, sizeof(value));
        (void)drain_result;
        continue;
      }
      if (fd == listen_fd_.get()) {
        AcceptNewConnections();
        continue;
      }
      HandleEvent(events[i]);
    }
  }
  return Status::Ok();
}

void EpollServer::AcceptNewConnections() {
  while (true) {
    const int client_fd = ::accept(listen_fd_.get(), nullptr, nullptr);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      if (errno == EINTR) continue;
      return;
    }

    auto conn = std::make_unique<Connection>(client_fd);
    epoll_event ev{};
    ev.events = WantedEvents(*conn);
    ev.data.fd = client_fd;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, client_fd, &ev) != 0) {
      CloseQuietly(client_fd);
      continue;
    }
    connections_.emplace(client_fd, std::move(conn));
  }
}

void EpollServer::HandleEvent(const epoll_event& ev) {
  const int fd = ev.data.fd;
  auto it = connections_.find(fd);
  if (it == connections_.end()) return;
  Connection& conn = *it->second;

  if (ev.events & (EPOLLHUP | EPOLLERR)) {
    RemoveConnection(fd);
    return;
  }

  if (ev.events & EPOLLIN) {
    conn.OnReadable(handler_);
  }
  if (!conn.closed() && (ev.events & EPOLLOUT)) {
    conn.OnWritable();
  }

  if (conn.closed()) {
    RemoveConnection(fd);
    return;
  }

  epoll_event new_ev{};
  new_ev.events = WantedEvents(conn);
  new_ev.data.fd = fd;
  ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, fd, &new_ev);
}

void EpollServer::RemoveConnection(int fd) {
  ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, fd, nullptr);
  CloseQuietly(fd);
  connections_.erase(fd);
}

}  // namespace neuralkv::net

#endif  // NEURALKV_LINUX
