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

// Parses one frame from the front of buffer. kComplete: consumed bytes are erased,
// out_type is set, and the matching out_* pointer is populated if non-null.
// kNeedMore: buffer untouched. kError: buffer cleared — caller should close the connection.
ParseResult TryParseFrame(std::vector<uint8_t>& buffer, ClientRequest* out_request,
                           ClientResponse* out_response, ClusterRequest* out_cluster_request,
                           ClusterResponse* out_cluster_response, MessageType* out_type);

// Convenience overload for callers that only ever see client frames
// (nkv-client, nkv-bench, and every existing test) — equivalent to
// passing nullptr for both cluster out-params.
ParseResult TryParseFrame(std::vector<uint8_t>& buffer, ClientRequest* out_request,
                           ClientResponse* out_response);

}  // namespace neuralkv::protocol
