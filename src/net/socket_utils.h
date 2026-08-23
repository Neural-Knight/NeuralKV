#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/result.h"
#include "common/status.h"

namespace neuralkv::net {

// Move-only owner of a POSIX file descriptor; closes it on destruction.
class Fd {
 public:
  Fd() noexcept = default;
  explicit Fd(int fd) noexcept;
  ~Fd();

  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;

  Fd(Fd&& other) noexcept;
  Fd& operator=(Fd&& other) noexcept;

  int get() const noexcept { return fd_; }
  bool valid() const noexcept { return fd_ >= 0; }

  // Transfers ownership to the caller; this wrapper no longer closes it.
  int release() noexcept;

 private:
  int fd_ = -1;
};

Status SetNonBlocking(int fd, bool non_blocking);

// Binds and listens on host:port (port 0 requests an ephemeral port; use
// getsockname on the returned fd to learn which one was assigned).
Result<int> TcpListen(const std::string& host, uint16_t port);

Result<int> TcpConnect(const std::string& host, uint16_t port);

// Loops on read()/write() until exactly len bytes have been transferred,
// retrying on EINTR. ReadFull treats EOF before len bytes as an error.
Status ReadFull(int fd, uint8_t* buf, std::size_t len);
Status WriteFull(int fd, const uint8_t* buf, std::size_t len);

void CloseQuietly(int fd);

}  // namespace neuralkv::net
