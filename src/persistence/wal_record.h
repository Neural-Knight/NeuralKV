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

// Reads one record starting at the current offset of fd.
//
// - Ok() with *has_record == true: a complete, CRC-valid record was read;
//   fd's offset now sits at the start of the next one.
// - Ok() with *has_record == false: fewer bytes remain than a full record
//   needs. This is the expected shape of a crash mid-write (the last
//   write() before a crash lands partially); callers should treat it as
//   the end of the log, not an error.
// - error: a record was fully present but its CRC, op, or field lengths
//   don't check out — real corruption rather than a truncated write, so
//   callers should refuse to proceed rather than guess at the data.
Status ReadNextWalRecord(int fd, WalRecord* record, bool* has_record);

}  // namespace neuralkv::persistence
