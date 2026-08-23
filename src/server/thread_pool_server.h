#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "common/status.h"
#include "concurrency/thread_pool.h"
#include "net/socket_utils.h"
#include "persistence/durable_storage.h"
#include "raft/node.h"
#include "server/handler.h"

namespace neuralkv {

// TCP server that hands each accepted connection to a fixed-size thread
// pool instead of serving it on the accept thread. Binds and listens
// during construction so port() is valid before Run() is called.
class ThreadPoolServer {
 public:
  ThreadPoolServer(std::string host, uint16_t port, persistence::DurableStorage& storage,
                    std::size_t num_workers, raft::RaftNode* raft = nullptr,
                    bool allow_stale_reads = false);

  // Accepts connections until Stop() is called (or a signal interrupts the
  // blocking accept()). Returns Ok() on a clean shutdown, or the error that
  // ended the loop early.
  Status Run();
  void Stop();

  uint16_t port() const { return port_; }

 private:
  std::string host_;
  uint16_t port_;
  RequestHandler handler_;
  ThreadPool pool_;
  std::atomic<bool> stop_{false};

  net::Fd listen_fd_;
  Status bind_status_ = Status::Ok();
};

}  // namespace neuralkv
