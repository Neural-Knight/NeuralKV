#include "server/handler.h"

#include <string>
#include <utility>

#include "common/result.h"
#include "common/status.h"
#include "raft/rpc_codec.h"

namespace neuralkv {

namespace {

protocol::ResponseStatus ToResponseStatus(ErrorCode code) {
  switch (code) {
    case ErrorCode::kOk:
      return protocol::ResponseStatus::kOk;
    case ErrorCode::kNotFound:
      return protocol::ResponseStatus::kNotFound;
    case ErrorCode::kInvalidArgument:
      return protocol::ResponseStatus::kBadRequest;
    case ErrorCode::kIOError:
    case ErrorCode::kInternal:
      return protocol::ResponseStatus::kInternalError;
  }
  return protocol::ResponseStatus::kInternalError;
}

}  // namespace

RequestHandler::RequestHandler(persistence::DurableStorage& storage, raft::RaftNode* raft,
                                bool allow_stale_reads)
    : storage_(storage), raft_(raft), allow_stale_reads_(allow_stale_reads) {}

bool RequestHandler::IsLeader() const {
  return raft_ == nullptr || raft_->state() == raft::RaftState::kLeader;
}

protocol::ClientResponse RequestHandler::Handle(const protocol::ClientRequest& req) {
  protocol::ClientResponse resp;
  resp.request_id = req.request_id;

  const bool is_write =
      req.opcode == protocol::Opcode::kSet || req.opcode == protocol::Opcode::kDelete;
  if (is_write && !IsLeader()) {
    resp.status = protocol::ResponseStatus::kWrongLeader;
    resp.leader_hint = raft_->leader_id();
    return resp;
  }

  switch (req.opcode) {
    case protocol::Opcode::kSet: {
      if (raft_ != nullptr) {
        raft::LogEntry entry;
        entry.op = persistence::WalOp::kSet;
        entry.key = req.key;
        entry.value = req.value;
        const Status status = raft_->Propose(std::move(entry));
        if (status.ok()) {
          resp.status = protocol::ResponseStatus::kOk;
        } else if (status.code() == ErrorCode::kInvalidArgument) {
          // Propose()'s specific signal for "not leader (any more)".
          resp.status = protocol::ResponseStatus::kWrongLeader;
          resp.leader_hint = raft_->leader_id();
        } else {
          resp.status = protocol::ResponseStatus::kInternalError;
        }
      } else {
        const Status status = storage_.Set(req.key, req.value);
        resp.status = ToResponseStatus(status.code());
      }
      break;
    }
    case protocol::Opcode::kGet: {
      if (raft_ != nullptr && !allow_stale_reads_) {
        // Linearizable read: a follower has nothing locally guaranteed current, so
        // the leader must confirm it still holds a live quorum (read_index-style)
        // before serving from local state.
        if (!IsLeader() || !raft_->ConfirmLeadershipQuorum()) {
          resp.status = protocol::ResponseStatus::kWrongLeader;
          resp.leader_hint = raft_->leader_id();
          break;
        }
      }
      Result<std::string> result = storage_.Get(req.key);
      if (result.ok()) {
        resp.status = protocol::ResponseStatus::kOk;
        resp.value = std::move(result).value();
      } else {
        resp.status = ToResponseStatus(result.status().code());
      }
      break;
    }
    case protocol::Opcode::kDelete: {
      if (raft_ != nullptr) {
        raft::LogEntry entry;
        entry.op = persistence::WalOp::kDelete;
        entry.key = req.key;
        const Status status = raft_->Propose(std::move(entry));
        if (status.ok()) {
          resp.status = protocol::ResponseStatus::kOk;
        } else if (status.code() == ErrorCode::kInvalidArgument) {
          resp.status = protocol::ResponseStatus::kWrongLeader;
          resp.leader_hint = raft_->leader_id();
        } else {
          resp.status = protocol::ResponseStatus::kInternalError;
        }
      } else {
        const Status status = storage_.Delete(req.key);
        resp.status = ToResponseStatus(status.code());
      }
      break;
    }
    default:
      resp.status = protocol::ResponseStatus::kBadRequest;
      break;
  }

  return resp;
}

protocol::ClusterResponse RequestHandler::HandleCluster(const protocol::ClusterRequest& req) {
  protocol::ClusterResponse resp;
  resp.request_id = req.request_id;

  switch (req.opcode) {
    case protocol::ClusterOpcode::kPing:
      resp.status = protocol::ResponseStatus::kOk;
      resp.body = "pong";
      break;
    case protocol::ClusterOpcode::kRequestVote: {
      if (raft_ == nullptr) {
        resp.status = protocol::ResponseStatus::kBadRequest;
        break;
      }
      raft::RequestVoteRequest rv_req;
      if (!raft::DecodeRequestVoteRequest(req.body, rv_req).ok()) {
        resp.status = protocol::ResponseStatus::kBadRequest;
        break;
      }
      std::string body;
      raft::EncodeRequestVoteResponse(raft_->HandleRequestVote(rv_req), body);
      resp.status = protocol::ResponseStatus::kOk;
      resp.body = std::move(body);
      break;
    }
    case protocol::ClusterOpcode::kAppendEntries: {
      if (raft_ == nullptr) {
        resp.status = protocol::ResponseStatus::kBadRequest;
        break;
      }
      raft::AppendEntriesRequest ae_req;
      if (!raft::DecodeAppendEntriesRequest(req.body, ae_req).ok()) {
        resp.status = protocol::ResponseStatus::kBadRequest;
        break;
      }
      std::string body;
      raft::EncodeAppendEntriesResponse(raft_->HandleAppendEntries(ae_req), body);
      resp.status = protocol::ResponseStatus::kOk;
      resp.body = std::move(body);
      break;
    }
    default:
      resp.status = protocol::ResponseStatus::kBadRequest;
      break;
  }

  return resp;
}

}  // namespace neuralkv
