#pragma once

#include <cstdint>
#include <string>

namespace neuralkv::protocol {

enum class MessageType : uint8_t {
  kClientRequest = 0x01,
  kClientResponse = 0x02,
};

enum class Opcode : uint8_t {
  kSet = 1,
  kGet = 2,
  kDelete = 3,
};

enum class ResponseStatus : uint16_t {
  kOk = 0,
  kNotFound = 1,
  kBadRequest = 2,
  kInternalError = 3,
  kWrongLeader = 4,
};

struct ClientRequest {
  uint64_t request_id = 0;
  Opcode opcode = Opcode::kGet;
  std::string key;
  std::string value;
};

struct ClientResponse {
  uint64_t request_id = 0;
  ResponseStatus status = ResponseStatus::kOk;
  std::string value;
};

// Outcome of an incremental parse attempt against a streaming byte buffer.
enum class ParseResult {
  kNeedMore,  // incomplete frame; caller should retain the buffer and read more
  kComplete,  // one full frame was consumed
  kError,     // malformed frame; the connection should be closed
};

}  // namespace neuralkv::protocol
