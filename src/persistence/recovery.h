#pragma once

#include <cstdint>
#include <string>

#include "common/status.h"
#include "storage/sharded_kv.h"

namespace neuralkv::persistence {

// Replays data_dir/wal/wal.log into kv in order, applying every complete
// record. A missing or empty WAL is not an error — a fresh node has
// nothing to recover, and last_applied_index is left at 0. Stops cleanly
// at a truncated tail record (the shape of a crash mid-write) but returns
// an error on CRC or validation failure, since that's real corruption
// rather than an in-progress write.
Status RecoverFromWal(const std::string& data_dir, ShardedKV& kv, uint64_t* last_applied_index);

}  // namespace neuralkv::persistence
