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

Status EncodeClusterRequest(const ClusterRequest& req, std::vector<uint8_t>& out);
Status EncodeClusterResponse(const ClusterResponse& resp, std::vector<uint8_t>& out);

Status DecodeClusterRequest(std::span<const uint8_t> payload, ClusterRequest& out);
Status DecodeClusterResponse(std::span<const uint8_t> payload, ClusterResponse& out);

// Attempts to parse one frame from the front of buffer. On kComplete, the
// consumed bytes are erased from buffer, out_type (if non-null) is set to
// the frame's message type, and exactly one of the four out_* data
// pointers is populated — whichever matches out_type — if the caller
// passed a non-null pointer for it. On kNeedMore, buffer is left
// untouched. On kError, buffer is cleared — the caller should close the
// connection rather than keep parsing it.
ParseResult TryParseFrame(std::vector<uint8_t>& buffer, ClientRequest* out_request,
                           ClientResponse* out_response, ClusterRequest* out_cluster_request,
                           ClusterResponse* out_cluster_response, MessageType* out_type);

// Convenience overload for callers that only ever see client frames
// (nkv-client, nkv-bench, and every existing test) — equivalent to
// passing nullptr for both cluster out-params.
ParseResult TryParseFrame(std::vector<uint8_t>& buffer, ClientRequest* out_request,
                           ClientResponse* out_response);

}  // namespace neuralkv::protocol
