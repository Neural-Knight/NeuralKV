#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/status.h"
#include "server/handler.h"

namespace neuralkv::net {

// Non-blocking per-connection state machine for edge-triggered event
// loops: OnReadable/OnWritable drain exactly what the socket has ready
// and buffer the rest, since a single edge-triggered notification won't
// fire again until more data arrives.
class Connection {
 public:
  explicit Connection(int fd);

  int fd() const { return fd_; }

  // Drains available input, dispatching every complete request to
  // handler and queuing its encoded response. Returns an error only for
  // a genuine I/O failure (EOF and EAGAIN are not errors); check closed()
  // for both cases.
  Status OnReadable(RequestHandler& handler);

  // Flushes as much of the queued output as the socket accepts.
  Status OnWritable();

  bool write_pending() const { return !write_buffer_.empty(); }
  std::size_t write_buffer_size() const { return write_buffer_.size(); }
  bool closed() const { return closed_; }
  void MarkClosed() { closed_ = true; }

 private:
  void ProcessFrames(RequestHandler& handler);

  int fd_;
  std::vector<uint8_t> read_buffer_;
  std::vector<uint8_t> write_buffer_;
  bool closed_ = false;
};

}  // namespace neuralkv::net
