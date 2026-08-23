#pragma once

#include "server/handler.h"

namespace neuralkv {

// Serves one connected client fd until the peer closes the connection or a
// protocol/I/O error occurs. Decodes each complete frame, dispatches it to
// handler, and writes back the encoded response. Does not close fd.
void ServeClientSession(int client_fd, RequestHandler& handler);

}  // namespace neuralkv
