#pragma once

#ifdef NEURALKV_LINUX

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <sys/epoll.h>

#include "common/status.h"
#include "net/connection.h"
#include "net/socket_utils.h"
#include "persistence/durable_storage.h"
#include "raft/node.h"
#include "server/handler.h"

namespace neuralkv::net {

// Single-threaded, edge-triggered epoll event loop: one thread owns the whole
// connection table, so per-connection state needs no locking. With Raft active,
// RaftNode::Propose() blocks this one thread until the write commits.
class EpollServer {
 public:
  EpollServer(std::string host, uint16_t port, persistence::DurableStorage& storage,
              raft::RaftNode* raft = nullptr, bool allow_stale_reads = false);

  // Runs the event loop until Stop() is called. Returns Ok() on a clean
  // shutdown, or the error that ended the loop early.
  Status Run();
  void Stop();

  uint16_t port() const { return port_; }

 private:
  void HandleEvent(const epoll_event& ev);
  void AcceptNewConnections();
  void RemoveConnection(int fd);

  std::string host_;
  uint16_t port_;
  RequestHandler handler_;
  std::atomic<bool> stop_{false};

  Fd listen_fd_;
  Fd epoll_fd_;
  Fd wake_fd_;
  Status bind_status_ = Status::Ok();
  std::unordered_map<int, std::unique_ptr<Connection>> connections_;
};

}  // namespace neuralkv::net

#endif  // NEURALKV_LINUX
