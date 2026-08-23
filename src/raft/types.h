#pragma once

#include <cstdint>
#include <string>

#include "persistence/wal_record.h"

namespace neuralkv::raft {

enum class RaftState { kFollower, kCandidate, kLeader };

// One log entry. Doubles as the on-disk WAL record for that index — see
// raft::Log, which is the only writer of a node's WAL once Raft is active.
struct LogEntry {
  uint64_t term = 0;
  uint64_t index = 0;
  persistence::WalOp op = persistence::WalOp::kSet;
  std::string key;
  std::string value;  // empty for DELETE
};

// current_term/voted_for must survive a restart: a node that forgets its
// term could vote twice in the same term, and a node that forgets its vote
// could grant a second one after a crash mid-election.
struct PersistentState {
  uint64_t current_term = 0;
  uint32_t voted_for = 0;  // 0 = none
};

}  // namespace neuralkv::raft
