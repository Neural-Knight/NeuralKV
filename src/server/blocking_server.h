#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "common/status.h"
#include "net/socket_utils.h"
#include "persistence/durable_storage.h"
#include "server/handler.h"

namespace neuralkv {

// Single-threaded, one-connection-at-a-time TCP server. Binds and listens
// during construction so port() is valid before Run() is called.
class BlockingServer {
 public:
  BlockingServer(std::string host, uint16_t port, persistence::DurableStorage& storage);

  // Serves connections until Stop() is called from another thread (or a
  // signal handler interrupts the blocking accept()). Returns Ok() on a
  // clean shutdown, or the error that ended the loop early.
  Status Run();
  void Stop();

  uint16_t port() const { return port_; }

 private:
  void ServeConnection(int fd);

  std::string host_;
  uint16_t port_;
  RequestHandler handler_;
  std::atomic<bool> stop_{false};

  net::Fd listen_fd_;
  Status bind_status_ = Status::Ok();
};

}  // namespace neuralkv
