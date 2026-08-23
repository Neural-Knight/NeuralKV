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

  // Closes the fd now (if valid) and forgets it, so the destructor won't
  // close it again. Use this to close from a different thread than the
  // one that owns the Fd.
  void reset() noexcept;

 private:
  int fd_ = -1;
};

Status SetNonBlocking(int fd, bool non_blocking);

// Binds and listens on host:port (port 0 requests an ephemeral port; use
// getsockname on the returned fd to learn which one was assigned).
Result<int> TcpListen(const std::string& host, uint16_t port);

Result<int> TcpConnect(const std::string& host, uint16_t port);

// Returns the port actually bound to fd via getsockname, or fallback if
// that fails. Needed after an ephemeral (port 0) bind to learn which port
// the OS assigned.
uint16_t GetBoundPort(int fd, uint16_t fallback);

// Loops on read()/write() until exactly len bytes have been transferred,
// retrying on EINTR. ReadFull treats EOF before len bytes as an error.
Status ReadFull(int fd, uint8_t* buf, std::size_t len);
Status WriteFull(int fd, const uint8_t* buf, std::size_t len);

void CloseQuietly(int fd);

}  // namespace neuralkv::net
