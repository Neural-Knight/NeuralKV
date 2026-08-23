#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/status.h"
#include "raft/types.h"

namespace neuralkv::raft {

struct RequestVoteRequest {
  uint64_t term = 0;
  uint32_t candidate_id = 0;
  uint64_t last_log_index = 0;
  uint64_t last_log_term = 0;
};

struct RequestVoteResponse {
  uint64_t term = 0;
  bool vote_granted = false;
};

struct AppendEntriesRequest {
  uint64_t term = 0;
  uint32_t leader_id = 0;
  uint64_t prev_log_index = 0;
  uint64_t prev_log_term = 0;
  uint64_t leader_commit = 0;
  std::vector<LogEntry> entries;
};

struct AppendEntriesResponse {
  uint64_t term = 0;
  bool success = false;
};

// Big-endian, no external serialization libs. Bodies are carried opaquely
// inside protocol::ClusterRequest::body / ClusterResponse::body.
Status EncodeRequestVoteRequest(const RequestVoteRequest& req, std::string& out);
Status DecodeRequestVoteRequest(const std::string& in, RequestVoteRequest& out);

Status EncodeRequestVoteResponse(const RequestVoteResponse& resp, std::string& out);
Status DecodeRequestVoteResponse(const std::string& in, RequestVoteResponse& out);

Status EncodeAppendEntriesRequest(const AppendEntriesRequest& req, std::string& out);
Status DecodeAppendEntriesRequest(const std::string& in, AppendEntriesRequest& out);

Status EncodeAppendEntriesResponse(const AppendEntriesResponse& resp, std::string& out);
Status DecodeAppendEntriesResponse(const std::string& in, AppendEntriesResponse& out);

}  // namespace neuralkv::raft
