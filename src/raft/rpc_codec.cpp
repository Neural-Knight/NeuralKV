#include "raft/rpc_codec.h"

#include <cstring>

namespace neuralkv::raft {

namespace {

void WriteU8(std::vector<uint8_t>& out, uint8_t value) { out.push_back(value); }

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

void WriteBytes(std::vector<uint8_t>& out, const std::string& bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
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

Status Truncated() {
  return Status::Error(ErrorCode::kInvalidArgument, "truncated raft RPC body");
}

// Cursor over a raw byte buffer with bounds-checked reads.
class Reader {
 public:
  explicit Reader(const std::string& in)
      : data_(reinterpret_cast<const uint8_t*>(in.data())), size_(in.size()) {}

  Status ReadU8(uint8_t* out) {
    if (offset_ + 1 > size_) return Truncated();
    *out = data_[offset_];
    offset_ += 1;
    return Status::Ok();
  }

  Status ReadU32(uint32_t* out) {
    if (offset_ + 4 > size_) return Truncated();
    *out = ReadU32BE(data_ + offset_);
    offset_ += 4;
    return Status::Ok();
  }

  Status ReadU64(uint64_t* out) {
    if (offset_ + 8 > size_) return Truncated();
    *out = ReadU64BE(data_ + offset_);
    offset_ += 8;
    return Status::Ok();
  }

  Status ReadBytes(uint32_t len, std::string* out) {
    if (offset_ + len > size_) return Truncated();
    out->assign(reinterpret_cast<const char*>(data_ + offset_), len);
    offset_ += len;
    return Status::Ok();
  }

  bool AtEnd() const { return offset_ == size_; }

 private:
  const uint8_t* data_;
  std::size_t size_;
  std::size_t offset_ = 0;
};

}  // namespace

Status EncodeRequestVoteRequest(const RequestVoteRequest& req, std::string& out) {
  std::vector<uint8_t> buf;
  WriteU64BE(buf, req.term);
  WriteU32BE(buf, req.candidate_id);
  WriteU64BE(buf, req.last_log_index);
  WriteU64BE(buf, req.last_log_term);
  out.assign(buf.begin(), buf.end());
  return Status::Ok();
}

Status DecodeRequestVoteRequest(const std::string& in, RequestVoteRequest& out) {
  Reader r(in);
  Status status = Status::Ok();
  if (!(status = r.ReadU64(&out.term)).ok()) return status;
  if (!(status = r.ReadU32(&out.candidate_id)).ok()) return status;
  if (!(status = r.ReadU64(&out.last_log_index)).ok()) return status;
  if (!(status = r.ReadU64(&out.last_log_term)).ok()) return status;
  if (!r.AtEnd()) return Status::Error(ErrorCode::kInvalidArgument, "trailing bytes in RequestVote request");
  return Status::Ok();
}

Status EncodeRequestVoteResponse(const RequestVoteResponse& resp, std::string& out) {
  std::vector<uint8_t> buf;
  WriteU64BE(buf, resp.term);
  WriteU8(buf, resp.vote_granted ? 1 : 0);
  out.assign(buf.begin(), buf.end());
  return Status::Ok();
}

Status DecodeRequestVoteResponse(const std::string& in, RequestVoteResponse& out) {
  Reader r(in);
  Status status = Status::Ok();
  if (!(status = r.ReadU64(&out.term)).ok()) return status;
  uint8_t granted = 0;
  if (!(status = r.ReadU8(&granted)).ok()) return status;
  out.vote_granted = granted != 0;
  if (!r.AtEnd()) return Status::Error(ErrorCode::kInvalidArgument, "trailing bytes in RequestVote response");
  return Status::Ok();
}

Status EncodeAppendEntriesRequest(const AppendEntriesRequest& req, std::string& out) {
  std::vector<uint8_t> buf;
  WriteU64BE(buf, req.term);
  WriteU32BE(buf, req.leader_id);
  WriteU64BE(buf, req.prev_log_index);
  WriteU64BE(buf, req.prev_log_term);
  WriteU64BE(buf, req.leader_commit);
  WriteU32BE(buf, static_cast<uint32_t>(req.entries.size()));
  for (const LogEntry& entry : req.entries) {
    WriteU64BE(buf, entry.term);
    WriteU64BE(buf, entry.index);
    WriteU8(buf, static_cast<uint8_t>(entry.op));
    WriteU32BE(buf, static_cast<uint32_t>(entry.key.size()));
    WriteBytes(buf, entry.key);
    WriteU32BE(buf, static_cast<uint32_t>(entry.value.size()));
    WriteBytes(buf, entry.value);
  }
  out.assign(buf.begin(), buf.end());
  return Status::Ok();
}

Status DecodeAppendEntriesRequest(const std::string& in, AppendEntriesRequest& out) {
  Reader r(in);
  Status status = Status::Ok();
  if (!(status = r.ReadU64(&out.term)).ok()) return status;
  if (!(status = r.ReadU32(&out.leader_id)).ok()) return status;
  if (!(status = r.ReadU64(&out.prev_log_index)).ok()) return status;
  if (!(status = r.ReadU64(&out.prev_log_term)).ok()) return status;
  if (!(status = r.ReadU64(&out.leader_commit)).ok()) return status;
  uint32_t entry_count = 0;
  if (!(status = r.ReadU32(&entry_count)).ok()) return status;

  out.entries.clear();
  out.entries.reserve(entry_count);
  for (uint32_t i = 0; i < entry_count; ++i) {
    LogEntry entry;
    if (!(status = r.ReadU64(&entry.term)).ok()) return status;
    if (!(status = r.ReadU64(&entry.index)).ok()) return status;
    uint8_t op_byte = 0;
    if (!(status = r.ReadU8(&op_byte)).ok()) return status;
    if (op_byte != static_cast<uint8_t>(persistence::WalOp::kSet) &&
        op_byte != static_cast<uint8_t>(persistence::WalOp::kDelete)) {
      return Status::Error(ErrorCode::kInvalidArgument, "unknown log entry op");
    }
    entry.op = static_cast<persistence::WalOp>(op_byte);
    uint32_t key_len = 0;
    if (!(status = r.ReadU32(&key_len)).ok()) return status;
    if (!(status = r.ReadBytes(key_len, &entry.key)).ok()) return status;
    uint32_t value_len = 0;
    if (!(status = r.ReadU32(&value_len)).ok()) return status;
    if (!(status = r.ReadBytes(value_len, &entry.value)).ok()) return status;
    out.entries.push_back(std::move(entry));
  }

  if (!r.AtEnd()) return Status::Error(ErrorCode::kInvalidArgument, "trailing bytes in AppendEntries request");
  return Status::Ok();
}

Status EncodeAppendEntriesResponse(const AppendEntriesResponse& resp, std::string& out) {
  std::vector<uint8_t> buf;
  WriteU64BE(buf, resp.term);
  WriteU8(buf, resp.success ? 1 : 0);
  out.assign(buf.begin(), buf.end());
  return Status::Ok();
}

Status DecodeAppendEntriesResponse(const std::string& in, AppendEntriesResponse& out) {
  Reader r(in);
  Status status = Status::Ok();
  if (!(status = r.ReadU64(&out.term)).ok()) return status;
  uint8_t success = 0;
  if (!(status = r.ReadU8(&success)).ok()) return status;
  out.success = success != 0;
  if (!r.AtEnd()) return Status::Error(ErrorCode::kInvalidArgument, "trailing bytes in AppendEntries response");
  return Status::Ok();
}

}  // namespace neuralkv::raft
