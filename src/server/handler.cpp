#include "server/handler.h"

#include <string>

#include "common/result.h"
#include "common/status.h"

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

RequestHandler::RequestHandler(persistence::DurableStorage& storage,
                                const cluster::ClusterConfig* cluster_config)
    : storage_(storage), cluster_config_(cluster_config) {}

bool RequestHandler::IsLeader() const {
  return cluster_config_ == nullptr || cluster_config_->local_node_id == cluster_config_->leader_node_id;
}

protocol::ClientResponse RequestHandler::Handle(const protocol::ClientRequest& req) {
  protocol::ClientResponse resp;
  resp.request_id = req.request_id;

  const bool is_write =
      req.opcode == protocol::Opcode::kSet || req.opcode == protocol::Opcode::kDelete;
  if (is_write && !IsLeader()) {
    resp.status = protocol::ResponseStatus::kWrongLeader;
    resp.leader_hint = cluster_config_->leader_node_id;
    return resp;
  }

  switch (req.opcode) {
    case protocol::Opcode::kSet: {
      const Status status = storage_.Set(req.key, req.value);
      resp.status = ToResponseStatus(status.code());
      break;
    }
    case protocol::Opcode::kGet: {
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
      const Status status = storage_.Delete(req.key);
      resp.status = ToResponseStatus(status.code());
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
    default:
      resp.status = protocol::ResponseStatus::kBadRequest;
      break;
  }

  return resp;
}

}  // namespace neuralkv
