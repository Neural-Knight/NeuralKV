#include "net/socket_utils.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace neuralkv::net {

Fd::Fd(int fd) noexcept : fd_(fd) {}

Fd::~Fd() { CloseQuietly(fd_); }

Fd::Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

Fd& Fd::operator=(Fd&& other) noexcept {
  if (this != &other) {
    CloseQuietly(fd_);
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

int Fd::release() noexcept {
  const int fd = fd_;
  fd_ = -1;
  return fd;
}

void CloseQuietly(int fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

Status SetNonBlocking(int fd, bool non_blocking) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return Status::Error(ErrorCode::kIOError, std::string("fcntl F_GETFL: ") + std::strerror(errno));
  }
  const int new_flags = non_blocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  if (::fcntl(fd, F_SETFL, new_flags) < 0) {
    return Status::Error(ErrorCode::kIOError, std::string("fcntl F_SETFL: ") + std::strerror(errno));
  }
  return Status::Ok();
}

Result<int> TcpListen(const std::string& host, uint16_t port) {
  struct addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  struct addrinfo* results = nullptr;
  const std::string port_str = std::to_string(port);
  const char* node = host.empty() ? nullptr : host.c_str();
  const int rc = ::getaddrinfo(node, port_str.c_str(), &hints, &results);
  if (rc != 0) {
    return Status::Error(ErrorCode::kIOError, std::string("getaddrinfo: ") + gai_strerror(rc));
  }

  Fd listen_fd;
  int last_errno = 0;
  for (struct addrinfo* p = results; p != nullptr; p = p->ai_next) {
    Fd candidate(::socket(p->ai_family, p->ai_socktype, p->ai_protocol));
    if (!candidate.valid()) {
      last_errno = errno;
      continue;
    }

    const int reuse = 1;
    ::setsockopt(candidate.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (::bind(candidate.get(), p->ai_addr, p->ai_addrlen) != 0) {
      last_errno = errno;
      continue;
    }
    listen_fd = std::move(candidate);
    break;
  }
  ::freeaddrinfo(results);

  if (!listen_fd.valid()) {
    return Status::Error(ErrorCode::kIOError, std::string("bind: ") + std::strerror(last_errno));
  }

  if (::listen(listen_fd.get(), 128) != 0) {
    return Status::Error(ErrorCode::kIOError, std::string("listen: ") + std::strerror(errno));
  }

  return listen_fd.release();
}

Result<int> TcpConnect(const std::string& host, uint16_t port) {
  struct addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* results = nullptr;
  const std::string port_str = std::to_string(port);
  const int rc = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &results);
  if (rc != 0) {
    return Status::Error(ErrorCode::kIOError, std::string("getaddrinfo: ") + gai_strerror(rc));
  }

  Fd conn_fd;
  int last_errno = 0;
  for (struct addrinfo* p = results; p != nullptr; p = p->ai_next) {
    Fd candidate(::socket(p->ai_family, p->ai_socktype, p->ai_protocol));
    if (!candidate.valid()) {
      last_errno = errno;
      continue;
    }
    if (::connect(candidate.get(), p->ai_addr, p->ai_addrlen) == 0) {
      conn_fd = std::move(candidate);
      break;
    }
    last_errno = errno;
  }
  ::freeaddrinfo(results);

  if (!conn_fd.valid()) {
    return Status::Error(ErrorCode::kIOError, std::string("connect: ") + std::strerror(last_errno));
  }

  return conn_fd.release();
}

Status ReadFull(int fd, uint8_t* buf, std::size_t len) {
  std::size_t total = 0;
  while (total < len) {
    const ssize_t n = ::read(fd, buf + total, len - total);
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::Error(ErrorCode::kIOError, std::string("read: ") + std::strerror(errno));
    }
    if (n == 0) {
      return Status::Error(ErrorCode::kIOError, "connection closed");
    }
    total += static_cast<std::size_t>(n);
  }
  return Status::Ok();
}

Status WriteFull(int fd, const uint8_t* buf, std::size_t len) {
  std::size_t total = 0;
  while (total < len) {
    const ssize_t n = ::write(fd, buf + total, len - total);
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::Error(ErrorCode::kIOError, std::string("write: ") + std::strerror(errno));
    }
    total += static_cast<std::size_t>(n);
  }
  return Status::Ok();
}

}  // namespace neuralkv::net
