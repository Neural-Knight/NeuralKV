#include "server/connection_session.h"

#include <cerrno>
#include <cstdint>
#include <vector>

#include <unistd.h>

#include "net/socket_utils.h"
#include "protocol/codec.h"

namespace neuralkv {

void ServeClientSession(int client_fd, RequestHandler& handler) {
  std::vector<uint8_t> read_buffer;
  uint8_t chunk[4096];

  while (true) {
    for (;;) {
      protocol::ClientRequest req;
      const protocol::ParseResult result = protocol::TryParseFrame(read_buffer, &req, nullptr);
      if (result == protocol::ParseResult::kNeedMore) break;
      if (result == protocol::ParseResult::kError) return;

      const protocol::ClientResponse resp = handler.Handle(req);
      std::vector<uint8_t> encoded;
      if (!protocol::EncodeClientResponse(resp, encoded).ok()) return;
      if (!net::WriteFull(client_fd, encoded.data(), encoded.size()).ok()) return;
    }

    const ssize_t n = ::read(client_fd, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (n == 0) return;  // peer closed the connection
    read_buffer.insert(read_buffer.end(), chunk, chunk + n);
  }
}

}  // namespace neuralkv
