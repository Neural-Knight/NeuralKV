#include "protocol/codec.h"

#include <utility>

#include "protocol/constants.h"

namespace neuralkv::protocol {

namespace {

uint16_t ReadU16BE(const uint8_t* p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) << 8 | p[1]);
}

uint32_t ReadU32BE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
         static_cast<uint32_t>(p[2]) << 8 | static_cast<uint32_t>(p[3]);
}

uint64_t ReadU64BE(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | p[i];
  }
  return value;
}

void WriteU16BE(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void WriteU32BE(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void WriteU64BE(std::vector<uint8_t>& out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(value >> shift));
  }
}

Status TruncatedPayload() {
  return Status::Error(ErrorCode::kInvalidArgument, "truncated payload");
}

}  // namespace

Status EncodeClientRequest(const ClientRequest& req, std::vector<uint8_t>& out) {
  if (req.key.empty()) {
    return Status::Error(ErrorCode::kInvalidArgument, "key must not be empty");
  }
  switch (req.opcode) {
    case Opcode::kSet:
      if (req.value.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "SET requires a non-empty value");
      }
      break;
    case Opcode::kGet:
    case Opcode::kDelete:
      if (!req.value.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "GET/DELETE must not carry a value");
      }
      break;
    default:
      return Status::Error(ErrorCode::kInvalidArgument, "unknown opcode");
  }

  const std::size_t payload_size = 8 + 1 + 4 + req.key.size() + 4 + req.value.size();
  if (payload_size > kMaxPayloadSize) {
    return Status::Error(ErrorCode::kInvalidArgument, "payload exceeds max size");
  }

  out.clear();
  out.reserve(kFrameHeaderSize + payload_size);
  WriteU16BE(out, kMagic);
  out.push_back(kProtocolVersion);
  out.push_back(static_cast<uint8_t>(MessageType::kClientRequest));
  WriteU32BE(out, static_cast<uint32_t>(payload_size));

  WriteU64BE(out, req.request_id);
  out.push_back(static_cast<uint8_t>(req.opcode));
  WriteU32BE(out, static_cast<uint32_t>(req.key.size()));
  out.insert(out.end(), req.key.begin(), req.key.end());
  WriteU32BE(out, static_cast<uint32_t>(req.value.size()));
  out.insert(out.end(), req.value.begin(), req.value.end());

  return Status::Ok();
}

Status EncodeClientResponse(const ClientResponse& resp, std::vector<uint8_t>& out) {
  const std::size_t payload_size = 8 + 2 + 4 + resp.value.size();
  if (payload_size > kMaxPayloadSize) {
    return Status::Error(ErrorCode::kInvalidArgument, "payload exceeds max size");
  }

  out.clear();
  out.reserve(kFrameHeaderSize + payload_size);
  WriteU16BE(out, kMagic);
  out.push_back(kProtocolVersion);
  out.push_back(static_cast<uint8_t>(MessageType::kClientResponse));
  WriteU32BE(out, static_cast<uint32_t>(payload_size));

  WriteU64BE(out, resp.request_id);
  WriteU16BE(out, static_cast<uint16_t>(resp.status));
  WriteU32BE(out, static_cast<uint32_t>(resp.value.size()));
  out.insert(out.end(), resp.value.begin(), resp.value.end());

  return Status::Ok();
}

Status DecodeClientRequest(std::span<const uint8_t> payload, ClientRequest& out) {
  std::size_t offset = 0;

  if (payload.size() < offset + 8) return TruncatedPayload();
  const uint64_t request_id = ReadU64BE(payload.data() + offset);
  offset += 8;

  if (payload.size() < offset + 1) return TruncatedPayload();
  const uint8_t opcode_byte = payload[offset];
  offset += 1;
  if (opcode_byte < static_cast<uint8_t>(Opcode::kSet) ||
      opcode_byte > static_cast<uint8_t>(Opcode::kDelete)) {
    return Status::Error(ErrorCode::kInvalidArgument, "unknown opcode");
  }
  const Opcode opcode = static_cast<Opcode>(opcode_byte);

  if (payload.size() < offset + 4) return TruncatedPayload();
  const uint32_t key_len = ReadU32BE(payload.data() + offset);
  offset += 4;
  if (key_len == 0) {
    return Status::Error(ErrorCode::kInvalidArgument, "key must not be empty");
  }
  if (payload.size() < offset + key_len) return TruncatedPayload();
  std::string key(reinterpret_cast<const char*>(payload.data() + offset), key_len);
  offset += key_len;

  if (payload.size() < offset + 4) return TruncatedPayload();
  const uint32_t value_len = ReadU32BE(payload.data() + offset);
  offset += 4;
  if (payload.size() < offset + value_len) return TruncatedPayload();
  std::string value(reinterpret_cast<const char*>(payload.data() + offset), value_len);
  offset += value_len;

  if (offset != payload.size()) {
    return Status::Error(ErrorCode::kInvalidArgument, "trailing bytes after payload");
  }

  switch (opcode) {
    case Opcode::kSet:
      if (value.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "SET requires a non-empty value");
      }
      break;
    case Opcode::kGet:
    case Opcode::kDelete:
      if (!value.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "GET/DELETE must not carry a value");
      }
      break;
  }

  out.request_id = request_id;
  out.opcode = opcode;
  out.key = std::move(key);
  out.value = std::move(value);
  return Status::Ok();
}

Status DecodeClientResponse(std::span<const uint8_t> payload, ClientResponse& out) {
  std::size_t offset = 0;

  if (payload.size() < offset + 8) return TruncatedPayload();
  const uint64_t request_id = ReadU64BE(payload.data() + offset);
  offset += 8;

  if (payload.size() < offset + 2) return TruncatedPayload();
  const uint16_t status_code = ReadU16BE(payload.data() + offset);
  offset += 2;
  if (status_code > static_cast<uint16_t>(ResponseStatus::kWrongLeader)) {
    return Status::Error(ErrorCode::kInvalidArgument, "unknown response status");
  }

  if (payload.size() < offset + 4) return TruncatedPayload();
  const uint32_t value_len = ReadU32BE(payload.data() + offset);
  offset += 4;
  if (payload.size() < offset + value_len) return TruncatedPayload();
  std::string value(reinterpret_cast<const char*>(payload.data() + offset), value_len);
  offset += value_len;

  if (offset != payload.size()) {
    return Status::Error(ErrorCode::kInvalidArgument, "trailing bytes after payload");
  }

  out.request_id = request_id;
  out.status = static_cast<ResponseStatus>(status_code);
  out.value = std::move(value);
  return Status::Ok();
}

ParseResult TryParseFrame(std::vector<uint8_t>& buffer, ClientRequest* out_request,
                           ClientResponse* out_response) {
  if (buffer.size() < kFrameHeaderSize) {
    return ParseResult::kNeedMore;
  }

  const uint16_t magic = ReadU16BE(buffer.data());
  if (magic != kMagic) {
    buffer.clear();
    return ParseResult::kError;
  }

  const uint8_t version = buffer[2];
  if (version != kProtocolVersion) {
    buffer.clear();
    return ParseResult::kError;
  }

  const uint8_t type_byte = buffer[3];
  const uint32_t length = ReadU32BE(buffer.data() + 4);
  if (length > kMaxPayloadSize) {
    buffer.clear();
    return ParseResult::kError;
  }

  const std::size_t frame_size = kFrameHeaderSize + length;
  if (buffer.size() < frame_size) {
    return ParseResult::kNeedMore;
  }

  const std::span<const uint8_t> payload(buffer.data() + kFrameHeaderSize, length);

  switch (static_cast<MessageType>(type_byte)) {
    case MessageType::kClientRequest: {
      ClientRequest req;
      Status status = DecodeClientRequest(payload, req);
      if (!status.ok()) {
        buffer.clear();
        return ParseResult::kError;
      }
      if (out_request != nullptr) {
        *out_request = std::move(req);
      }
      break;
    }
    case MessageType::kClientResponse: {
      ClientResponse resp;
      Status status = DecodeClientResponse(payload, resp);
      if (!status.ok()) {
        buffer.clear();
        return ParseResult::kError;
      }
      if (out_response != nullptr) {
        *out_response = std::move(resp);
      }
      break;
    }
    default:
      buffer.clear();
      return ParseResult::kError;
  }

  buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(frame_size));
  return ParseResult::kComplete;
}

}  // namespace neuralkv::protocol
