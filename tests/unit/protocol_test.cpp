#include "protocol/codec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "protocol/constants.h"
#include "protocol/types.h"

namespace neuralkv::protocol {
namespace {

void AppendU16BE(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void AppendU32BE(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void AppendU64BE(std::vector<uint8_t>& out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(value >> shift));
  }
}

void AppendBytes(std::vector<uint8_t>& out, std::string_view bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}

// Builds a complete frame from a hand-assembled payload, independent of the
// codec under test, so header/endianness bugs can't hide behind it.
std::vector<uint8_t> BuildFrame(uint16_t magic, uint8_t version, MessageType type,
                                 const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> frame;
  AppendU16BE(frame, magic);
  frame.push_back(version);
  frame.push_back(static_cast<uint8_t>(type));
  AppendU32BE(frame, static_cast<uint32_t>(payload.size()));
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

std::vector<uint8_t> BuildRequestPayload(uint64_t request_id, Opcode opcode,
                                          std::string_view key, std::string_view value) {
  std::vector<uint8_t> payload;
  AppendU64BE(payload, request_id);
  payload.push_back(static_cast<uint8_t>(opcode));
  AppendU32BE(payload, static_cast<uint32_t>(key.size()));
  AppendBytes(payload, key);
  AppendU32BE(payload, static_cast<uint32_t>(value.size()));
  AppendBytes(payload, value);
  return payload;
}

// --- Round-trip -------------------------------------------------------

TEST(ProtocolCodecTest, RoundTripSetRequest) {
  ClientRequest req{.request_id = 42, .opcode = Opcode::kSet, .key = "k1", .value = "v1"};
  std::vector<uint8_t> encoded;
  ASSERT_TRUE(EncodeClientRequest(req, encoded).ok());

  ClientRequest decoded;
  ASSERT_TRUE(
      DecodeClientRequest(std::span(encoded).subspan(kFrameHeaderSize), decoded).ok());
  EXPECT_EQ(decoded.request_id, 42u);
  EXPECT_EQ(decoded.opcode, Opcode::kSet);
  EXPECT_EQ(decoded.key, "k1");
  EXPECT_EQ(decoded.value, "v1");
}

TEST(ProtocolCodecTest, RoundTripGetRequest) {
  ClientRequest req{.request_id = 7, .opcode = Opcode::kGet, .key = "k1", .value = ""};
  std::vector<uint8_t> encoded;
  ASSERT_TRUE(EncodeClientRequest(req, encoded).ok());

  ClientRequest decoded;
  ASSERT_TRUE(
      DecodeClientRequest(std::span(encoded).subspan(kFrameHeaderSize), decoded).ok());
  EXPECT_EQ(decoded.request_id, 7u);
  EXPECT_EQ(decoded.opcode, Opcode::kGet);
  EXPECT_EQ(decoded.key, "k1");
  EXPECT_TRUE(decoded.value.empty());
}

TEST(ProtocolCodecTest, RoundTripDeleteRequest) {
  ClientRequest req{.request_id = 99, .opcode = Opcode::kDelete, .key = "gone", .value = ""};
  std::vector<uint8_t> encoded;
  ASSERT_TRUE(EncodeClientRequest(req, encoded).ok());

  ClientRequest decoded;
  ASSERT_TRUE(
      DecodeClientRequest(std::span(encoded).subspan(kFrameHeaderSize), decoded).ok());
  EXPECT_EQ(decoded.opcode, Opcode::kDelete);
  EXPECT_EQ(decoded.key, "gone");
}

TEST(ProtocolCodecTest, RoundTripOkResponseWithValue) {
  ClientResponse resp{.request_id = 5, .status = ResponseStatus::kOk, .value = "hello"};
  std::vector<uint8_t> encoded;
  ASSERT_TRUE(EncodeClientResponse(resp, encoded).ok());

  ClientResponse decoded;
  ASSERT_TRUE(
      DecodeClientResponse(std::span(encoded).subspan(kFrameHeaderSize), decoded).ok());
  EXPECT_EQ(decoded.request_id, 5u);
  EXPECT_EQ(decoded.status, ResponseStatus::kOk);
  EXPECT_EQ(decoded.value, "hello");
}

TEST(ProtocolCodecTest, RoundTripNotFoundResponse) {
  ClientResponse resp{.request_id = 6, .status = ResponseStatus::kNotFound, .value = ""};
  std::vector<uint8_t> encoded;
  ASSERT_TRUE(EncodeClientResponse(resp, encoded).ok());

  ClientResponse decoded;
  ASSERT_TRUE(
      DecodeClientResponse(std::span(encoded).subspan(kFrameHeaderSize), decoded).ok());
  EXPECT_EQ(decoded.status, ResponseStatus::kNotFound);
  EXPECT_TRUE(decoded.value.empty());
}

// --- Endianness ---------------------------------------------------------

TEST(ProtocolCodecTest, HandCraftedFrameDecodesBigEndianFields) {
  const std::vector<uint8_t> payload =
      BuildRequestPayload(0x0102030405060708ULL, Opcode::kSet, "key", "value");
  std::vector<uint8_t> buffer = BuildFrame(kMagic, kProtocolVersion,
                                            MessageType::kClientRequest, payload);

  ClientRequest req;
  ClientResponse resp;
  ASSERT_EQ(TryParseFrame(buffer, &req, &resp), ParseResult::kComplete);
  EXPECT_EQ(req.request_id, 0x0102030405060708ULL);
  EXPECT_EQ(req.opcode, Opcode::kSet);
  EXPECT_EQ(req.key, "key");
  EXPECT_EQ(req.value, "value");
}

// --- Partial reads -------------------------------------------------------

TEST(TryParseFrameTest, EmptyBufferNeedsMore) {
  std::vector<uint8_t> buffer;
  ClientRequest req;
  EXPECT_EQ(TryParseFrame(buffer, &req, nullptr), ParseResult::kNeedMore);
  EXPECT_TRUE(buffer.empty());
}

TEST(TryParseFrameTest, PartialHeaderNeedsMoreAndBufferUnchanged) {
  std::vector<uint8_t> buffer = {0x4E, 0x4B, 0x01};  // 3 of 8 header bytes
  ClientRequest req;
  EXPECT_EQ(TryParseFrame(buffer, &req, nullptr), ParseResult::kNeedMore);
  EXPECT_EQ(buffer.size(), 3u);
}

TEST(TryParseFrameTest, CompleteHeaderIncompletePayloadNeedsMoreAndBufferUnchanged) {
  const std::vector<uint8_t> payload =
      BuildRequestPayload(1, Opcode::kGet, "key", "");
  std::vector<uint8_t> full_frame =
      BuildFrame(kMagic, kProtocolVersion, MessageType::kClientRequest, payload);
  std::vector<uint8_t> buffer(full_frame.begin(), full_frame.begin() + kFrameHeaderSize + 2);

  ClientRequest req;
  EXPECT_EQ(TryParseFrame(buffer, &req, nullptr), ParseResult::kNeedMore);
  EXPECT_EQ(buffer.size(), kFrameHeaderSize + 2);
}

TEST(TryParseFrameTest, CompleteFrameConsumedFromFront) {
  const std::vector<uint8_t> payload = BuildRequestPayload(1, Opcode::kGet, "key", "");
  std::vector<uint8_t> buffer =
      BuildFrame(kMagic, kProtocolVersion, MessageType::kClientRequest, payload);

  ClientRequest req;
  EXPECT_EQ(TryParseFrame(buffer, &req, nullptr), ParseResult::kComplete);
  EXPECT_EQ(req.key, "key");
  EXPECT_TRUE(buffer.empty());
}

TEST(TryParseFrameTest, TwoCompleteFramesParsedSequentially) {
  const std::vector<uint8_t> first_payload = BuildRequestPayload(1, Opcode::kGet, "first", "");
  const std::vector<uint8_t> second_payload =
      BuildRequestPayload(2, Opcode::kGet, "second", "");
  std::vector<uint8_t> buffer =
      BuildFrame(kMagic, kProtocolVersion, MessageType::kClientRequest, first_payload);
  const std::vector<uint8_t> second_frame =
      BuildFrame(kMagic, kProtocolVersion, MessageType::kClientRequest, second_payload);
  buffer.insert(buffer.end(), second_frame.begin(), second_frame.end());

  ClientRequest req;
  ASSERT_EQ(TryParseFrame(buffer, &req, nullptr), ParseResult::kComplete);
  EXPECT_EQ(req.key, "first");

  ASSERT_EQ(TryParseFrame(buffer, &req, nullptr), ParseResult::kComplete);
  EXPECT_EQ(req.key, "second");
  EXPECT_TRUE(buffer.empty());
}

TEST(TryParseFrameTest, RemainingBytesStayForNextParse) {
  const std::vector<uint8_t> first_payload = BuildRequestPayload(1, Opcode::kGet, "first", "");
  std::vector<uint8_t> buffer =
      BuildFrame(kMagic, kProtocolVersion, MessageType::kClientRequest, first_payload);
  const std::vector<uint8_t> trailing_partial = {0x4E, 0x4B, 0x01};
  buffer.insert(buffer.end(), trailing_partial.begin(), trailing_partial.end());

  ClientRequest req;
  ASSERT_EQ(TryParseFrame(buffer, &req, nullptr), ParseResult::kComplete);
  EXPECT_EQ(buffer.size(), trailing_partial.size());
  EXPECT_EQ(TryParseFrame(buffer, &req, nullptr), ParseResult::kNeedMore);
  EXPECT_EQ(buffer.size(), trailing_partial.size());
}

// --- Malformed input -------------------------------------------------------

TEST(TryParseFrameTest, BadMagicReturnsError) {
  const std::vector<uint8_t> payload = BuildRequestPayload(1, Opcode::kGet, "key", "");
  std::vector<uint8_t> buffer =
      BuildFrame(0x0000, kProtocolVersion, MessageType::kClientRequest, payload);

  ClientRequest req;
  EXPECT_EQ(TryParseFrame(buffer, &req, nullptr), ParseResult::kError);
}

TEST(TryParseFrameTest, BadVersionReturnsError) {
  const std::vector<uint8_t> payload = BuildRequestPayload(1, Opcode::kGet, "key", "");
  std::vector<uint8_t> buffer =
      BuildFrame(kMagic, 0xFF, MessageType::kClientRequest, payload);

  ClientRequest req;
  EXPECT_EQ(TryParseFrame(buffer, &req, nullptr), ParseResult::kError);
}

TEST(TryParseFrameTest, LengthExceedingMaxReturnsError) {
  std::vector<uint8_t> buffer;
  AppendU16BE(buffer, kMagic);
  buffer.push_back(kProtocolVersion);
  buffer.push_back(static_cast<uint8_t>(MessageType::kClientRequest));
  AppendU32BE(buffer, kMaxPayloadSize + 1);

  ClientRequest req;
  EXPECT_EQ(TryParseFrame(buffer, &req, nullptr), ParseResult::kError);
}

TEST(TryParseFrameTest, SetWithEmptyValueReturnsError) {
  const std::vector<uint8_t> payload = BuildRequestPayload(1, Opcode::kSet, "key", "");
  std::vector<uint8_t> buffer =
      BuildFrame(kMagic, kProtocolVersion, MessageType::kClientRequest, payload);

  ClientRequest req;
  EXPECT_EQ(TryParseFrame(buffer, &req, nullptr), ParseResult::kError);
}

TEST(TryParseFrameTest, GetWithNonZeroValueLenReturnsError) {
  const std::vector<uint8_t> payload = BuildRequestPayload(1, Opcode::kGet, "key", "unexpected");
  std::vector<uint8_t> buffer =
      BuildFrame(kMagic, kProtocolVersion, MessageType::kClientRequest, payload);

  ClientRequest req;
  EXPECT_EQ(TryParseFrame(buffer, &req, nullptr), ParseResult::kError);
}

TEST(TryParseFrameTest, EmptyKeyReturnsError) {
  const std::vector<uint8_t> payload = BuildRequestPayload(1, Opcode::kGet, "", "");
  std::vector<uint8_t> buffer =
      BuildFrame(kMagic, kProtocolVersion, MessageType::kClientRequest, payload);

  ClientRequest req;
  EXPECT_EQ(TryParseFrame(buffer, &req, nullptr), ParseResult::kError);
}

TEST(TryParseFrameTest, TruncatedPayloadAfterHeaderNeedsMoreNotError) {
  const std::vector<uint8_t> payload = BuildRequestPayload(1, Opcode::kSet, "key", "value");
  std::vector<uint8_t> full_frame =
      BuildFrame(kMagic, kProtocolVersion, MessageType::kClientRequest, payload);
  std::vector<uint8_t> buffer(full_frame.begin(), full_frame.end() - 1);

  ClientRequest req;
  EXPECT_EQ(TryParseFrame(buffer, &req, nullptr), ParseResult::kNeedMore);
  EXPECT_EQ(buffer.size(), full_frame.size() - 1);
}

// --- Encode-level validation ----------------------------------------------

TEST(ProtocolCodecTest, EncodeRejectsEmptyKey) {
  ClientRequest req{.request_id = 1, .opcode = Opcode::kGet, .key = "", .value = ""};
  std::vector<uint8_t> out;
  Status status = EncodeClientRequest(req, out);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), ErrorCode::kInvalidArgument);
}

TEST(ProtocolCodecTest, EncodeRejectsSetWithEmptyValue) {
  ClientRequest req{.request_id = 1, .opcode = Opcode::kSet, .key = "k", .value = ""};
  std::vector<uint8_t> out;
  Status status = EncodeClientRequest(req, out);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), ErrorCode::kInvalidArgument);
}

TEST(ProtocolCodecTest, EncodeRejectsGetWithValue) {
  ClientRequest req{.request_id = 1, .opcode = Opcode::kGet, .key = "k", .value = "v"};
  std::vector<uint8_t> out;
  Status status = EncodeClientRequest(req, out);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), ErrorCode::kInvalidArgument);
}

}  // namespace
}  // namespace neuralkv::protocol
