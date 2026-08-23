#include "common/status.h"

#include <utility>

namespace neuralkv {

Status::Status(ErrorCode code, std::string message)
    : code_(code), message_(std::move(message)) {}

Status Status::Ok() { return Status(ErrorCode::kOk, ""); }

Status Status::Error(ErrorCode code, std::string message) {
  return Status(code, std::move(message));
}

}  // namespace neuralkv
