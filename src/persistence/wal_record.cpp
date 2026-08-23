#include "persistence/wal_record.h"

#include <cerrno>

#include <unistd.h>

#include "persistence/crc32.h"

namespace neuralkv::persistence {

namespace {

// crc(4) + term(8) + index(8) + op(1) + key_len(4)
constexpr std::size_t kFixedHeaderSize = 25;
// Sanity bound on a single field's declared length: garbage decoded from a
// corrupt record can claim an arbitrary 32-bit length, and without a cap a
// read of that "length" from a short file could look like a clean
// truncated tail instead of the corruption it is.
constexpr uint32_t kMaxFieldSize = 64u * 1024 * 1024;

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

uint32_t ReadU32BE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
         static_cast<uint32_t>(p[2]) << 8 | static_cast<uint32_t>(p[3]);
}

uint64_t ReadU64BE(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<uint64_t>(p[i]);
  }
  return value;
}

// Reads up to len bytes, retrying on EINTR. Returns fewer than len only
// when the file ended first — for a regular file, a short read() is
// itself the EOF signal.
std::size_t ReadSome(int fd, uint8_t* buf, std::size_t len) {
  std::size_t total = 0;
  while (total < len) {
    const ssize_t n = ::read(fd, buf + total, len - total);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    total += static_cast<std::size_t>(n);
  }
  return total;
}

Status CorruptRecord(const char* why) {
  return Status::Error(ErrorCode::kIOError, std::string("wal record corrupt: ") + why);
}

}  // namespace

void EncodeWalRecord(const WalRecord& record, std::vector<uint8_t>& out) {
  std::vector<uint8_t> body;
  WriteU64BE(body, record.term);
  WriteU64BE(body, record.index);
  body.push_back(static_cast<uint8_t>(record.op));
  WriteU32BE(body, static_cast<uint32_t>(record.key.size()));
  body.insert(body.end(), record.key.begin(), record.key.end());
  WriteU32BE(body, static_cast<uint32_t>(record.value.size()));
  body.insert(body.end(), record.value.begin(), record.value.end());

  WriteU32BE(out, Crc32(body.data(), body.size()));
  out.insert(out.end(), body.begin(), body.end());
}

Status ReadNextWalRecord(int fd, WalRecord* record, bool* has_record) {
  *has_record = false;

  uint8_t header[kFixedHeaderSize];
  const std::size_t header_read = ReadSome(fd, header, sizeof(header));
  if (header_read < sizeof(header)) return Status::Ok();  // clean EOF or truncated tail

  const uint32_t crc = ReadU32BE(header);
  const uint64_t term = ReadU64BE(header + 4);
  const uint64_t index = ReadU64BE(header + 12);
  const WalOp op = static_cast<WalOp>(header[20]);
  const uint32_t key_len = ReadU32BE(header + 21);
  if (key_len > kMaxFieldSize) return CorruptRecord("key length out of range");

  std::vector<uint8_t> key(key_len);
  if (!key.empty() && ReadSome(fd, key.data(), key.size()) < key.size()) {
    return Status::Ok();  // truncated tail
  }

  uint8_t value_len_buf[4];
  if (ReadSome(fd, value_len_buf, sizeof(value_len_buf)) < sizeof(value_len_buf)) {
    return Status::Ok();  // truncated tail
  }
  const uint32_t value_len = ReadU32BE(value_len_buf);
  if (value_len > kMaxFieldSize) return CorruptRecord("value length out of range");

  std::vector<uint8_t> value(value_len);
  if (!value.empty() && ReadSome(fd, value.data(), value.size()) < value.size()) {
    return Status::Ok();  // truncated tail
  }

  std::vector<uint8_t> body;
  body.insert(body.end(), header + 4, header + kFixedHeaderSize);
  body.insert(body.end(), key.begin(), key.end());
  body.insert(body.end(), value_len_buf, value_len_buf + sizeof(value_len_buf));
  body.insert(body.end(), value.begin(), value.end());
  if (Crc32(body.data(), body.size()) != crc) return CorruptRecord("CRC mismatch");

  if (op != WalOp::kSet && op != WalOp::kDelete) return CorruptRecord("unknown op");
  if (key.empty()) return CorruptRecord("empty key");
  if (op == WalOp::kSet && value.empty()) return CorruptRecord("SET with empty value");

  record->term = term;
  record->index = index;
  record->op = op;
  record->key.assign(key.begin(), key.end());
  record->value.assign(value.begin(), value.end());
  *has_record = true;
  return Status::Ok();
}

}  // namespace neuralkv::persistence
