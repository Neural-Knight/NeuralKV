#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/status.h"

namespace neuralkv::persistence {

enum class WalOp : uint8_t { kSet = 1, kDelete = 2 };

// One durable mutation. term/index are placeholders for the Raft log this
// WAL will back later: term is always 0 until then, and index is assigned
// by WalWriter when the record is appended.
struct WalRecord {
  uint64_t term = 0;
  uint64_t index = 0;
  WalOp op = WalOp::kSet;
  std::string key;
  std::string value;  // empty for kDelete
};

// Appends the on-disk encoding of record to out: crc32 + term + index + op
// + key_len + key + value_len + value, with every multi-byte field
// big-endian and the crc covering everything after itself.
void EncodeWalRecord(const WalRecord& record, std::vector<uint8_t>& out);

// Reads one record at fd's current offset. Ok()+has_record=true: valid record
// read, offset advanced. Ok()+has_record=false: a truncated tail (crash mid-write) —
// treat as end of log, not an error. Error: CRC/field mismatch — real corruption, stop.
Status ReadNextWalRecord(int fd, WalRecord* record, bool* has_record);

}  // namespace neuralkv::persistence
