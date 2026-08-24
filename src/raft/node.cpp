#include "raft/node.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <random>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "protocol/types.h"

namespace neuralkv::raft {

namespace {

constexpr std::chrono::milliseconds kTickInterval{15};
constexpr std::chrono::milliseconds kHeartbeatInterval{75};
constexpr std::chrono::milliseconds kElectionTimeoutMin{250};
constexpr std::chrono::milliseconds kElectionTimeoutMax{400};
constexpr std::chrono::milliseconds kProposeTimeout{3000};

void WriteU32BE(uint8_t* p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value >> 24);
  p[1] = static_cast<uint8_t>(value >> 16);
  p[2] = static_cast<uint8_t>(value >> 8);
  p[3] = static_cast<uint8_t>(value);
}

void WriteU64BE(uint8_t* p, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<uint8_t>(value >> (56 - 8 * i));
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

}  // namespace

RaftNode::RaftNode(uint32_t local_node_id, const cluster::ClusterConfig& config,
                    persistence::DurableStorage& storage, cluster::ClusterTransport& transport)
    : local_node_id_(local_node_id),
      config_(config),
      storage_(storage),
      transport_(transport),
      log_(storage.wal_writer(), storage.data_dir()),
      state_path_(storage.data_dir() + "/raft/state.bin") {
  open_status_ = LoadPersistedState();
  if (!open_status_.ok()) return;
  open_status_ = log_.Load();
  if (!open_status_.ok()) return;
  ResetElectionDeadlineLocked();
}

RaftNode::~RaftNode() { Stop(); }

void RaftNode::Start() {
  if (!open_status_.ok() || started_.exchange(true)) return;
  run_thread_ = std::thread([this] { RunLoop(); });
}

void RaftNode::Stop() {
  stop_.store(true);
  if (run_thread_.joinable()) run_thread_.join();
}

std::chrono::milliseconds RaftNode::RandomElectionTimeout() {
  static thread_local std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<int> dist(static_cast<int>(kElectionTimeoutMin.count()),
                                           static_cast<int>(kElectionTimeoutMax.count()));
  return std::chrono::milliseconds(dist(rng));
}

void RaftNode::ResetElectionDeadlineLocked() {
  election_deadline_ = std::chrono::steady_clock::now() + RandomElectionTimeout();
}

Status RaftNode::LoadPersistedState() {
  const int fd = ::open(state_path_.c_str(), O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) return Status::Ok();  // fresh node: term 0, no vote
    return Status::Error(ErrorCode::kIOError, std::string("open ") + state_path_ + ": " + std::strerror(errno));
  }

  uint8_t buf[12];
  std::size_t total = 0;
  while (total < sizeof(buf)) {
    const ssize_t n = ::read(fd, buf + total, sizeof(buf) - total);
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return Status::Error(ErrorCode::kIOError, std::string("read ") + state_path_ + ": " + std::strerror(errno));
    }
    if (n == 0) break;
    total += static_cast<std::size_t>(n);
  }
  ::close(fd);

  if (total != sizeof(buf)) {
    return Status::Error(ErrorCode::kIOError, "raft state file truncated: " + state_path_);
  }
  current_term_ = ReadU64BE(buf);
  voted_for_ = ReadU32BE(buf + 8);
  return Status::Ok();
}

void RaftNode::PersistStateLocked() {
  const std::string raft_dir = storage_.data_dir() + "/raft";
  ::mkdir(raft_dir.c_str(), 0755);  // ignore EEXIST; a real failure surfaces on open() below

  const int fd = ::open(state_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return;  // best-effort; a lost persist just risks re-voting after a crash

  uint8_t buf[12];
  WriteU64BE(buf, current_term_);
  WriteU32BE(buf + 8, voted_for_);

  std::size_t written = 0;
  while (written < sizeof(buf)) {
    const ssize_t n = ::write(fd, buf + written, sizeof(buf) - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    written += static_cast<std::size_t>(n);
  }
  ::fsync(fd);
  ::close(fd);
}

RaftState RaftNode::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

uint64_t RaftNode::current_term() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_term_;
}

uint64_t RaftNode::commit_index() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return commit_index_;
}

uint64_t RaftNode::last_applied() const { return storage_.last_applied_index(); }

uint32_t RaftNode::leader_id() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return leader_id_;
}

uint64_t RaftNode::lag_entries() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return commit_index_ - storage_.last_applied_index();
}

uint64_t RaftNode::replication_lag_entries(uint32_t peer_id) const {
  if (peer_id == local_node_id_) return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != RaftState::kLeader) return 0;
  const auto it = match_index_.find(peer_id);
  if (it == match_index_.end()) return 0;
  const uint64_t last_index = log_.LastIndex();
  return last_index > it->second ? last_index - it->second : 0;
}

void RaftNode::BecomeFollowerLocked(uint64_t term) {
  current_term_ = term;
  voted_for_ = 0;
  state_ = RaftState::kFollower;
  leader_id_ = 0;
  last_ack_time_.clear();
  PersistStateLocked();
  ResetElectionDeadlineLocked();
}

void RaftNode::BecomeLeaderLocked() {
  state_ = RaftState::kLeader;
  leader_id_ = local_node_id_;
  next_index_.clear();
  match_index_.clear();
  last_ack_time_.clear();
  for (const cluster::PeerInfo& peer : config_.peers) {
    if (peer.node_id == local_node_id_) continue;
    next_index_[peer.node_id] = log_.LastIndex() + 1;
    match_index_[peer.node_id] = 0;
  }
  next_heartbeat_ = std::chrono::steady_clock::now();  // send the first round immediately
}

// Majority of peers (plus this node itself) acked within the last
// heartbeat interval, at the current term — cheap enough to call from
// every GET, since it's just scanning an already-maintained map.
bool RaftNode::HasRecentMajorityContactLocked() const {
  if (state_ != RaftState::kLeader) return false;
  const auto now = std::chrono::steady_clock::now();
  int contacted = 1;  // self
  for (const auto& [node_id, when] : last_ack_time_) {
    if (now - when <= kHeartbeatInterval) ++contacted;
  }
  const int majority = static_cast<int>(config_.peers.size()) / 2 + 1;
  return contacted >= majority;
}

void RaftNode::ApplyCommittedEntriesLocked() {
  while (storage_.last_applied_index() < commit_index_) {
    const uint64_t next = storage_.last_applied_index() + 1;
    const LogEntry* entry = log_.Get(next);
    if (entry == nullptr) break;  // shouldn't happen: commit_index_ never exceeds a replicated index
    persistence::WalRecord record{
        .term = entry->term, .index = entry->index, .op = entry->op, .key = entry->key, .value = entry->value};
    storage_.ApplyCommitted(record);
  }
  cv_.notify_all();
}

RequestVoteResponse RaftNode::HandleRequestVote(const RequestVoteRequest& req) {
  std::lock_guard<std::mutex> lock(mutex_);
  RequestVoteResponse resp;

  if (req.term > current_term_) {
    BecomeFollowerLocked(req.term);
  }
  resp.term = current_term_;

  if (req.term < current_term_) {
    resp.vote_granted = false;
    return resp;
  }

  const bool can_vote = voted_for_ == 0 || voted_for_ == req.candidate_id;
  const bool challenger_log_at_least_as_up_to_date =
      req.last_log_term > log_.LastTerm() ||
      (req.last_log_term == log_.LastTerm() && req.last_log_index >= log_.LastIndex());

  if (can_vote && challenger_log_at_least_as_up_to_date) {
    voted_for_ = req.candidate_id;
    PersistStateLocked();
    ResetElectionDeadlineLocked();  // granting a vote counts as hearing from a legitimate peer
    resp.vote_granted = true;
  } else {
    resp.vote_granted = false;
  }
  return resp;
}

AppendEntriesResponse RaftNode::HandleAppendEntries(const AppendEntriesRequest& req) {
  std::lock_guard<std::mutex> lock(mutex_);
  AppendEntriesResponse resp;

  if (req.term < current_term_) {
    resp.term = current_term_;
    resp.success = false;
    return resp;
  }
  if (req.term > current_term_) {
    BecomeFollowerLocked(req.term);
  } else if (state_ == RaftState::kCandidate) {
    state_ = RaftState::kFollower;  // another candidate already won this term
  }
  leader_id_ = req.leader_id;
  ResetElectionDeadlineLocked();
  resp.term = current_term_;

  if (req.prev_log_index > 0) {
    const LogEntry* prev = log_.Get(req.prev_log_index);
    if (prev == nullptr || prev->term != req.prev_log_term) {
      resp.success = false;
      return resp;
    }
  }

  uint64_t index = req.prev_log_index;
  for (const LogEntry& entry : req.entries) {
    ++index;
    const LogEntry* existing = log_.Get(index);
    if (existing != nullptr && existing->term == entry.term) {
      continue;  // already have this exact entry
    }
    if (existing != nullptr) {
      log_.TruncateFrom(index);  // conflicting entry and everything after it must go
    }
    LogEntry to_append = entry;
    log_.Append(to_append);
  }

  if (req.leader_commit > commit_index_) {
    commit_index_ = std::min(req.leader_commit, log_.LastIndex());
    ApplyCommittedEntriesLocked();
  }

  resp.success = true;
  return resp;
}

Status RaftNode::Propose(LogEntry entry) {
  uint64_t index;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RaftState::kLeader) {
      return Status::Error(ErrorCode::kInvalidArgument, "not leader");
    }
    entry.term = current_term_;
    const Status status = log_.Append(entry);
    if (!status.ok()) return status;
    index = entry.index;
  }

  // Replicate immediately rather than waiting for the next heartbeat tick;
  // the background loop keeps retrying afterward regardless.
  for (const cluster::PeerInfo& peer : config_.peers) {
    if (peer.node_id != local_node_id_) ReplicateToPeer(peer);
  }

  std::unique_lock<std::mutex> lock(mutex_);
  const bool reached = cv_.wait_for(lock, kProposeTimeout, [&] {
    return storage_.last_applied_index() >= index || state_ != RaftState::kLeader;
  });
  if (!reached) {
    return Status::Error(ErrorCode::kIOError, "propose timed out waiting for commit");
  }
  if (storage_.last_applied_index() < index) {
    return Status::Error(ErrorCode::kInvalidArgument, "not leader");
  }
  return Status::Ok();
}

bool RaftNode::ConfirmLeadershipQuorum() {
  uint64_t term;
  std::vector<cluster::PeerInfo> peers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RaftState::kLeader) return false;
    // Amortization: ordinary heartbeats already prove liveness every
    // kHeartbeatInterval; skip this round's RPCs entirely if that proof
    // is still fresh rather than re-doing it on every single GET.
    if (HasRecentMajorityContactLocked()) return true;
    term = current_term_;
    peers = config_.peers;
  }

  int acks = 1;  // this node's own term is current by construction
  const int majority = static_cast<int>(peers.size()) / 2 + 1;

  for (const cluster::PeerInfo& peer : peers) {
    if (peer.node_id == local_node_id_) continue;

    uint64_t prev_log_index;
    uint64_t prev_log_term;
    uint64_t leader_commit;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (state_ != RaftState::kLeader || current_term_ != term) return false;
      const uint64_t next_idx = next_index_[peer.node_id];
      prev_log_index = next_idx - 1;
      const LogEntry* prev = prev_log_index == 0 ? nullptr : log_.Get(prev_log_index);
      prev_log_term = prev != nullptr ? prev->term : 0;
      leader_commit = commit_index_;
    }

    // Deliberately empty entries: this round exists to prove quorum
    // reachability at the current term, not to replicate — a concurrent
    // heartbeat or Propose() call handles actual replication and updates
    // next_index_/match_index_ on its own.
    const AppendEntriesRequest req{.term = term,
                                    .leader_id = local_node_id_,
                                    .prev_log_index = prev_log_index,
                                    .prev_log_term = prev_log_term,
                                    .leader_commit = leader_commit,
                                    .entries = {}};
    std::string body;
    if (!EncodeAppendEntriesRequest(req, body).ok()) continue;
    const protocol::ClusterRequest cluster_req{
        .request_id = 0, .opcode = protocol::ClusterOpcode::kAppendEntries, .body = body};
    const Result<protocol::ClusterResponse> result = transport_.SendRpc(peer, cluster_req);
    if (!result.ok()) continue;  // unreachable this round; doesn't count toward the majority

    AppendEntriesResponse resp;
    if (!DecodeAppendEntriesResponse(result.value().body, resp).ok()) continue;

    std::lock_guard<std::mutex> lock(mutex_);
    if (resp.term > current_term_) {
      BecomeFollowerLocked(resp.term);
      return false;
    }
    if (state_ != RaftState::kLeader || current_term_ != term) return false;
    if (resp.success) {
      ++acks;
      last_ack_time_[peer.node_id] = std::chrono::steady_clock::now();
    }
  }

  return acks >= majority;
}

void RaftNode::AdvanceCommitIndexLocked() {
  std::vector<uint64_t> indices;
  indices.push_back(log_.LastIndex());  // leader's own log is always fully up to date
  for (const auto& [node_id, match] : match_index_) {
    indices.push_back(match);
  }
  std::sort(indices.begin(), indices.end());

  const std::size_t majority = indices.size() / 2 + 1;
  const uint64_t candidate = indices[indices.size() - majority];
  if (candidate <= commit_index_) return;

  // Raft safety: only commit an entry from a prior term indirectly, by
  // committing a later current-term entry that covers it in the log.
  const LogEntry* entry = log_.Get(candidate);
  if (entry != nullptr && entry->term == current_term_) {
    commit_index_ = candidate;
    ApplyCommittedEntriesLocked();
  }
}

void RaftNode::ReplicateToPeer(const cluster::PeerInfo& peer) {
  uint64_t term;
  uint64_t prev_log_index;
  uint64_t prev_log_term;
  uint64_t leader_commit;
  std::vector<LogEntry> entries;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RaftState::kLeader) return;
    term = current_term_;
    const uint64_t next_idx = next_index_[peer.node_id];
    prev_log_index = next_idx - 1;
    const LogEntry* prev = prev_log_index == 0 ? nullptr : log_.Get(prev_log_index);
    prev_log_term = prev != nullptr ? prev->term : 0;
    entries = log_.EntriesFrom(next_idx);
    leader_commit = commit_index_;
  }

  AppendEntriesRequest req{.term = term,
                            .leader_id = local_node_id_,
                            .prev_log_index = prev_log_index,
                            .prev_log_term = prev_log_term,
                            .leader_commit = leader_commit,
                            .entries = entries};
  std::string body;
  if (!EncodeAppendEntriesRequest(req, body).ok()) return;

  const protocol::ClusterRequest cluster_req{
      .request_id = 0, .opcode = protocol::ClusterOpcode::kAppendEntries, .body = body};
  const Result<protocol::ClusterResponse> result = transport_.SendRpc(peer, cluster_req);
  if (!result.ok()) return;  // peer unreachable this round; retried next tick

  AppendEntriesResponse resp;
  if (!DecodeAppendEntriesResponse(result.value().body, resp).ok()) return;

  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != RaftState::kLeader || current_term_ != term) return;  // stale by the time the reply arrived
  if (resp.term > current_term_) {
    BecomeFollowerLocked(resp.term);
    return;
  }

  if (resp.success) {
    last_ack_time_[peer.node_id] = std::chrono::steady_clock::now();
    if (!entries.empty()) {
      match_index_[peer.node_id] = entries.back().index;
      next_index_[peer.node_id] = entries.back().index + 1;
    }
    AdvanceCommitIndexLocked();
  } else if (next_index_[peer.node_id] > 1) {
    --next_index_[peer.node_id];
  }
}

void RaftNode::StartElection() {
  uint64_t term;
  uint64_t last_log_index;
  uint64_t last_log_term;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++current_term_;
    term = current_term_;
    voted_for_ = local_node_id_;
    state_ = RaftState::kCandidate;
    leader_id_ = 0;
    PersistStateLocked();
    ResetElectionDeadlineLocked();
    last_log_index = log_.LastIndex();
    last_log_term = log_.LastTerm();
  }

  const RequestVoteRequest req{.term = term,
                                .candidate_id = local_node_id_,
                                .last_log_index = last_log_index,
                                .last_log_term = last_log_term};
  std::string body;
  if (!EncodeRequestVoteRequest(req, body).ok()) return;
  const protocol::ClusterRequest cluster_req{
      .request_id = 0, .opcode = protocol::ClusterOpcode::kRequestVote, .body = body};

  int votes = 1;  // vote for self
  const int majority = static_cast<int>(config_.peers.size()) / 2 + 1;

  for (const cluster::PeerInfo& peer : config_.peers) {
    if (peer.node_id == local_node_id_) continue;

    const Result<protocol::ClusterResponse> result = transport_.SendRpc(peer, cluster_req);
    if (!result.ok()) continue;

    RequestVoteResponse resp;
    if (!DecodeRequestVoteResponse(result.value().body, resp).ok()) continue;

    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RaftState::kCandidate || current_term_ != term) return;  // no longer a live candidacy
    if (resp.term > current_term_) {
      BecomeFollowerLocked(resp.term);
      return;
    }
    if (resp.vote_granted) ++votes;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == RaftState::kCandidate && current_term_ == term && votes >= majority) {
    BecomeLeaderLocked();
  }
}

void RaftNode::RunLoop() {
  while (!stop_.load()) {
    std::this_thread::sleep_for(kTickInterval);
    if (stop_.load()) break;

    RaftState current_state;
    bool election_timed_out = false;
    bool send_heartbeat = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      current_state = state_;
      const auto now = std::chrono::steady_clock::now();
      if (current_state != RaftState::kLeader && now >= election_deadline_) {
        election_timed_out = true;
      }
      if (current_state == RaftState::kLeader && now >= next_heartbeat_) {
        send_heartbeat = true;
        next_heartbeat_ = now + kHeartbeatInterval;
      }
    }

    if (send_heartbeat) {
      for (const cluster::PeerInfo& peer : config_.peers) {
        if (peer.node_id != local_node_id_) ReplicateToPeer(peer);
      }
    } else if (election_timed_out) {
      StartElection();
    }
  }
}

}  // namespace neuralkv::raft
