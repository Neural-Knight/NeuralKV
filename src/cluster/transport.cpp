#include "cluster/transport.h"

#include <vector>

#include <unistd.h>

#include "net/socket_utils.h"
#include "protocol/codec.h"

namespace neuralkv::cluster {

ClusterTransport::ClusterTransport(uint32_t local_node_id) : local_node_id_(local_node_id) {}

ClusterTransport::~ClusterTransport() { CloseAll(); }

Result<int> ClusterTransport::GetOrConnect(const PeerInfo& peer) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = peer_fds_.find(peer.node_id);
  if (it != peer_fds_.end()) return it->second;

  Result<int> conn = net::TcpConnect(peer.host, peer.port);
  if (!conn.ok()) return conn.status();

  const int fd = conn.value();
  peer_fds_.emplace(peer.node_id, fd);
  return fd;
}

Result<protocol::ClusterResponse> ClusterTransport::SendRpc(const PeerInfo& peer,
                                                             const protocol::ClusterRequest& req) {
  Result<int> conn = net::TcpConnect(peer.host, peer.port);
  if (!conn.ok()) return conn.status();
  const int fd = conn.value();

  std::vector<uint8_t> encoded;
  Status status = protocol::EncodeClusterRequest(req, encoded);
  if (!status.ok()) {
    net::CloseQuietly(fd);
    return status;
  }

  status = net::WriteFull(fd, encoded.data(), encoded.size());
  if (!status.ok()) {
    net::CloseQuietly(fd);
    return status;
  }

  std::vector<uint8_t> buffer;
  uint8_t chunk[4096];
  protocol::ClusterResponse resp;
  protocol::MessageType frame_type = protocol::MessageType::kClusterResponse;
  while (true) {
    const protocol::ParseResult result =
        protocol::TryParseFrame(buffer, nullptr, nullptr, nullptr, &resp, &frame_type);
    if (result == protocol::ParseResult::kComplete) {
      net::CloseQuietly(fd);
      if (frame_type != protocol::MessageType::kClusterResponse) {
        return Status::Error(ErrorCode::kIOError, "peer sent a non-cluster-response frame");
      }
      return resp;
    }
    if (result == protocol::ParseResult::kError) {
      net::CloseQuietly(fd);
      return Status::Error(ErrorCode::kIOError, "malformed cluster response from peer");
    }

    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n <= 0) {
      net::CloseQuietly(fd);
      return Status::Error(ErrorCode::kIOError, "connection closed while awaiting cluster response");
    }
    buffer.insert(buffer.end(), chunk, chunk + n);
  }
}

void ClusterTransport::CloseAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [node_id, fd] : peer_fds_) {
    net::CloseQuietly(fd);
  }
  peer_fds_.clear();
}

}  // namespace neuralkv::cluster
