#pragma once

#include <cstdint>
#include <string>

namespace neuralkv {

enum class ErrorCode : uint16_t {
  kOk = 0,
  kNotFound,
  kInvalidArgument,
  kIOError,
  kInternal,
};

// Represents the outcome of an operation that can fail for operational
// reasons (bad input, missing key, disk error). Not used for programmer
// errors, which should still assert or terminate.
class Status {
 public:
  static Status Ok();
  static Status Error(ErrorCode code, std::string message = "");

  bool ok() const { return code_ == ErrorCode::kOk; }
  ErrorCode code() const { return code_; }
  const std::string& message() const { return message_; }

 private:
  Status(ErrorCode code, std::string message);

  ErrorCode code_;
  std::string message_;
};

}  // namespace neuralkv
