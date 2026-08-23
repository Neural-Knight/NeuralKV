#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "common/status.h"
#include "protocol/types.h"

namespace neuralkv::protocol {

Status EncodeClientRequest(const ClientRequest& req, std::vector<uint8_t>& out);
Status EncodeClientResponse(const ClientResponse& resp, std::vector<uint8_t>& out);

Status DecodeClientRequest(std::span<const uint8_t> payload, ClientRequest& out);
Status DecodeClientResponse(std::span<const uint8_t> payload, ClientResponse& out);

// Attempts to parse one frame from the front of buffer. On kComplete, the
// consumed bytes are erased from buffer and exactly one of out_request /
// out_response is populated based on the frame's message type. On
// kNeedMore, buffer is left untouched. On kError, buffer is cleared — the
// caller should close the connection rather than keep parsing it.
ParseResult TryParseFrame(std::vector<uint8_t>& buffer, ClientRequest* out_request,
                           ClientResponse* out_response);

}  // namespace neuralkv::protocol
