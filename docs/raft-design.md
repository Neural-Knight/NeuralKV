# Raft Design

How NeuralKV replicates SET/DELETE across a cluster: leader election, log
replication, commit/apply, and how all of that sits on top of the
existing WAL. Follows the Raft consensus algorithm (Ongaro & Ousterhout,
"In Search of an Understandable Consensus Algorithm").

## Components

- `raft::RaftNode` (`src/raft/node.h`/`.cpp`) — the state machine: current
  term, role (follower/candidate/leader), election timer, heartbeat/
  replication loop, and the RequestVote/AppendEntries handlers.
- `raft::Log` (`src/raft/log.h`/`.cpp`) — the node's log, backed directly
  by its WAL (`persistence::WalWriter`). Not internally synchronized;
  `RaftNode` serializes all access with its own mutex — so its writes
  never actually contend with anything else for `WalWriter`'s group-commit
  batching (see wal-design.md).
- `raft::rpc_codec` — big-endian wire encoding for RequestVote/
  AppendEntries bodies, carried opaquely inside
  `protocol::ClusterRequest`/`ClusterResponse` (`MessageType::kClusterRequest`
  /`kClusterResponse`, `ClusterOpcode::kRequestVote`/`kAppendEntries`) —
  the same TCP port and framing every other cluster RPC already uses.
- `cluster::ClusterTransport` — dials a fresh connection per RPC and
  closes it immediately after reading the response (no per-peer cache —
  see Known limitations); `RaftNode` uses it to send RequestVote/
  AppendEntries and reads the replies synchronously.
- `RequestHandler` — routes client SET/DELETE through
  `RaftNode::Propose()` when a `RaftNode` is present, rejects them with
  `kWrongLeader` + the current leader's id otherwise. GET is linearizable
  by default (see Linearizable reads below) unless `--allow-stale-reads`
  is set, in which case it always reads local storage directly.

## Election

Each node starts as a follower with a randomized election timeout
(250–400ms). A follower or candidate that hears nothing valid for that
long increments its term, votes for itself, and requests votes from
every peer (`RequestVoteRequest{term, candidate_id, last_log_index,
last_log_term}`). A vote is granted only if the requester's term is at
least as current and its log is at least as up to date (§5.4.1 of the
paper — compares last-log term first, then index). A candidate that wins
a majority (including its own vote) becomes leader and immediately sends
empty AppendEntries (heartbeats) to establish itself; anyone whose term
turns out to be stale reverts to follower and adopts the higher term.

Randomized timeouts are the anti-split-vote mechanism: with three nodes
and independently rolled timeouts, one candidate almost always starts
its election well before the others.

## Replication

The leader tracks `nextIndex`/`matchIndex` per follower. Each heartbeat
tick (75ms) or immediately after a new `Propose()`, it sends
`AppendEntriesRequest{term, leader_id, prev_log_index, prev_log_term,
leader_commit, entries}` — `entries` is whatever the follower is missing
(empty for a pure heartbeat). A follower rejects if its log doesn't have
`prev_log_index`/`prev_log_term` (the log-matching check); on acceptance
it truncates any conflicting suffix (different term at the same index)
and appends the leader's entries in its place (`Log::TruncateFrom` +
`Log::Append`).

On a successful reply the leader advances that follower's
`matchIndex`/`nextIndex`; on rejection it retries from one entry earlier
next round. A rejected AppendEntries because of a stale term causes the
leader to step down immediately.

## Commit and apply

The leader recomputes its commit index after every successful reply: sort
`matchIndex` values (including its own log length) and take the
majority-order entry, but only actually advance `commitIndex` if that
entry belongs to the leader's *current* term — the standard Raft safety
rule against committing an old-term entry by indirect majority alone
(§5.4.2). A follower advances its own `commitIndex` to
`min(leaderCommit, its own last log index)` whenever an AppendEntries
carries a higher one.

Applying is synchronous with whichever of those two paths just moved
`commitIndex`: every entry between the old and new commit index is handed
to `persistence::DurableStorage::ApplyCommitted`, which touches only the
in-memory `ShardedKV` (the entry's WAL write already happened when it was
appended to the log — no double write). `RaftNode::Propose()` blocks the
calling client-handler thread until its own entry's index has been
applied on this node (bounded by a 3s timeout), so a client that gets
`OK` back knows its write is durable on a majority and visible locally.

## WAL integration

`raft::Log` doesn't keep a separate log file — it *is* the WAL. Every
`Log::Append` writes straight through `WalWriter::Append` (now carrying
the entry's real Raft term, not the fixed 0 a single-node write used),
then `Sync`s it durable before returning — the same group-commit path a
single-node write uses. `Log::TruncateFrom` handles the one thing an
append-only file can't do incrementally: dropping a conflicting suffix.
It rebuilds the surviving prefix in memory and calls
`WalWriter::RewriteAll`, which truncates the file to empty and rewrites
just those records, fsync'd. This is a full-file rewrite rather than an
in-place edit — acceptable at this scale, and it keeps the on-disk format
identical to a single-node WAL, byte for byte.

On startup, `RaftNode` reads `data_dir/raft/state.bin` (current term +
voted-for, 12 bytes, fsync'd on every change) and has `Log::Load()`
replay the WAL into memory. `DurableStorage`'s own recovery pass already
replayed the WAL into the KV store separately — `last_applied_index()`
tracks how far that got, and the Raft apply loop picks up from there
without redoing already-applied work.

## Linearizable reads

GET is linearizable by default. A follower always rejects with
`kWrongLeader` (there's nothing it can serve locally with a currency
guarantee); a leader confirms it still holds a live quorum before
reading. That confirmation is `RaftNode::ConfirmLeadershipQuorum()`, a
simplified `read_index` (§6.4 of the paper): the leader sends one round
of empty `AppendEntries` to every peer at its current term and requires a
majority (including itself) to succeed before trusting its own local
state. A `true` result means no other leader has been elected since the
round started, so every entry a client could have observed as committed
is already reflected locally — this node's own commit/apply path is
synchronous, so `last_applied` never lags `commit_index` by the time a
write's `Propose()` call returns.

`ConfirmLeadershipQuorum()` skips that round trip entirely if a majority
of peers already acked an AppendEntries (heartbeat or otherwise) within
the last heartbeat interval (75ms) at the current term — tracked in
`last_ack_time_`, cleared on every leadership/term change. This is still
exactly as safe: a superseding leader would have needed to win an
election inside that same window, which requires a round of
vote-granting that would have reached this node and demoted it first.
When contact isn't recent enough, GET falls back to the full round.

`--allow-stale-reads` on `nkv-server` reverts to legacy stale-read mode:
every GET, on any node, reads local storage immediately with no quorum
check — possibly stale on a lagging follower, but with no extra RPC cost
on the leader either.

## Metrics

`RaftNode` exposes two accessors, both computed from state it already
tracks — no separate bookkeeping, no HTTP endpoint yet:

- `lag_entries()` — `commit_index() - last_applied()` on any node. Settles
  back to 0 immediately after every commit, since apply runs synchronously
  with whatever just advanced `commit_index`; a sustained non-zero value
  would mean the apply loop itself is stuck, not just behind.
- `replication_lag_entries(peer_id)` — leader-only: that peer's
  `matchIndex` subtracted from the leader's own last log index. 0 on a
  follower/candidate (no `matchIndex` map to read) and 0 for the local
  node id.

## Known limitations

- **No snapshots.** A restarted or far-behind node replays/receives the
  *entire* log; there's no InstallSnapshot RPC to compact it. See
  "Snapshots: deferred" below.
- **Propose() blocks the calling thread**, including epoll's single event
  loop thread when running in `--io epoll` mode — the same tradeoff
  DurableStorage's fsync already introduces, just with a Raft round trip
  layered on top of the fsync it still does. `ConfirmLeadershipQuorum()`
  carries the identical tradeoff for GET when it can't skip its round trip.
- **No connect/read timeouts in ClusterTransport.** Each RPC pays a fresh
  TCP handshake (no per-peer connection cache — see Components), and a
  hung (not dead) peer can still stall a replication round on the
  `connect()`/`read()` calls until the OS's own TCP-level failure
  detection kicks in. A cleanly killed process's socket closes
  immediately, so this hasn't been an issue in testing, but it's not a
  bound guarantee.
- **Fixed 3-way majority arithmetic** — no dynamic membership changes; the
  peer list a node starts with is the peer list it runs with.
- **No systematic fault injection.** `src/testing/fault_injection.h`
  (`FaultInjectingTransport`) is a small test-only stub — drop outbound
  RPCs to a peer, or simulate a two-node partition by dropping both
  directions — used by one in-process test to confirm replication
  resumes once a fault heals. It has no scripted fault schedules, no
  latency injection, no message reordering; a real harness for that is
  its own future pass.

## Snapshots: deferred

No `InstallSnapshot` RPC and no on-disk snapshot format exist yet. A
restarted or far-behind node always recovers by reading the *entire* WAL
(`DurableStorage`'s own recovery scan) and then, if further behind than
that, receiving the rest of the log via ordinary `AppendEntries` batches
from the leader — `raft_catchup_test.cpp` exercises exactly this path for
100 entries and it's fast at that scale. The gap only matters once a
log's on-disk size or a follower's catch-up distance gets large enough
that full-log replay becomes the dominant cost. Planned trigger
thresholds: snapshot when the log exceeds ~10,000
entries or the WAL file exceeds ~64 MB, whichever comes first, mirroring
`Log::TruncateFrom`'s existing full-file-rewrite approach (`WalWriter::
RewriteAll`) but writing a compacted `ShardedKV` state dump instead of a
truncated log. A lagging follower whose required `prev_log_index` no
longer exists in the leader's (now-compacted) log would receive that
snapshot instead of a full replay.
