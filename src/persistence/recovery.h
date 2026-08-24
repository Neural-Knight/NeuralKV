#pragma once

#include <cstdint>
#include <string>

#include "common/status.h"
#include "storage/sharded_kv.h"

namespace neuralkv::persistence {

// Replays data_dir/wal/wal.log into kv in order. A missing/empty WAL isn't an
// error — a fresh node has nothing to recover. Stops cleanly at a truncated tail
// (crash mid-write) but errors on CRC/validation failure, since that's real corruption.
Status RecoverFromWal(const std::string& data_dir, ShardedKV& kv, uint64_t* last_applied_index);

}  // namespace neuralkv::persistence
