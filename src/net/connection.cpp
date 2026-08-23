#include "net/connection.h"

#include <cerrno>
#include <cstring>

#include <unistd.h>

#include "net/socket_utils.h"
#include "protocol/codec.h"

namespace neuralkv::net {

Connection::Connection(int fd) : fd_(fd) { SetNonBlocking(fd_, true); }

Status Connection::OnReadable(RequestHandler& handler) {
  uint8_t chunk[4096];
  while (true) {
    const ssize_t n = ::read(fd_, chunk, sizeof(chunk));
    if (n > 0) {
      read_buffer_.insert(read_buffer_.end(), chunk, chunk + n);
      continue;
    }
    if (n == 0) {
      MarkClosed();
      break;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // drained for now
    if (errno == EINTR) continue;
    MarkClosed();
    return Status::Error(ErrorCode::kIOError, std::string("read: ") + std::strerror(errno));
  }

  ProcessFrames(handler);
  return Status::Ok();
}

Status Connection::OnWritable() {
  while (!write_buffer_.empty()) {
    const ssize_t n = ::write(fd_, write_buffer_.data(), write_buffer_.size());
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      MarkClosed();
      return Status::Error(ErrorCode::kIOError, std::string("write: ") + std::strerror(errno));
    }
    write_buffer_.erase(write_buffer_.begin(), write_buffer_.begin() + n);
  }
  return Status::Ok();
}

void Connection::ProcessFrames(RequestHandler& handler) {
  while (true) {
    protocol::ClientRequest req;
    protocol::ClusterRequest cluster_req;
    protocol::MessageType type;
    const protocol::ParseResult result =
        protocol::TryParseFrame(read_buffer_, &req, nullptr, &cluster_req, nullptr, &type);
    if (result == protocol::ParseResult::kNeedMore) break;
    if (result == protocol::ParseResult::kError) {
      MarkClosed();
      return;
    }

    std::vector<uint8_t> encoded;
    if (type == protocol::MessageType::kClientRequest) {
      if (!protocol::EncodeClientResponse(handler.Handle(req), encoded).ok()) {
        MarkClosed();
        return;
      }
    } else if (type == protocol::MessageType::kClusterRequest) {
      if (!protocol::EncodeClusterResponse(handler.HandleCluster(cluster_req), encoded).ok()) {
        MarkClosed();
        return;
      }
    } else {
      MarkClosed();  // a response-typed frame has no business arriving here
      return;
    }
    write_buffer_.insert(write_buffer_.end(), encoded.begin(), encoded.end());
  }
}

}  // namespace neuralkv::net
