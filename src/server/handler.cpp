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

RequestHandler::RequestHandler(ShardedKV& kv) : kv_(kv) {}

protocol::ClientResponse RequestHandler::Handle(const protocol::ClientRequest& req) {
  protocol::ClientResponse resp;
  resp.request_id = req.request_id;

  switch (req.opcode) {
    case protocol::Opcode::kSet: {
      const Status status = kv_.Set(req.key, req.value);
      resp.status = ToResponseStatus(status.code());
      break;
    }
    case protocol::Opcode::kGet: {
      Result<std::string> result = kv_.Get(req.key);
      if (result.ok()) {
        resp.status = protocol::ResponseStatus::kOk;
        resp.value = std::move(result).value();
      } else {
        resp.status = ToResponseStatus(result.status().code());
      }
      break;
    }
    case protocol::Opcode::kDelete: {
      const Status status = kv_.Delete(req.key);
      resp.status = ToResponseStatus(status.code());
      break;
    }
    default:
      resp.status = protocol::ResponseStatus::kBadRequest;
      break;
  }

  return resp;
}

}  // namespace neuralkv
