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
  `RaftNode` serializes all access with its own mutex.
- `raft::rpc_codec` — big-endian wire encoding for RequestVote/
  AppendEntries bodies, carried opaquely inside
  `protocol::ClusterRequest`/`ClusterResponse` (`MessageType::kClusterRequest`
  /`kClusterResponse`, `ClusterOpcode::kRequestVote`/`kAppendEntries`) —
  the same TCP port and framing every other cluster RPC already uses.
- `cluster::ClusterTransport` — dials and caches one outbound connection
  per peer; `RaftNode` uses it to send RequestVote/AppendEntries and reads
  the replies synchronously.
- `RequestHandler` — routes client SET/DELETE through
  `RaftNode::Propose()` when a `RaftNode` is present, rejects them with
  `kWrongLeader` + the current leader's id otherwise; GET always reads
  local storage directly.

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
the entry's real Raft term, not the fixed 0 a single-node write used) and
fsyncs before returning. `Log::TruncateFrom` handles the one thing an
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

## Known limitations

- **No snapshots.** A restarted or far-behind node replays/receives the
  *entire* log; there's no InstallSnapshot RPC to compact it. Fine at this
  scale, a real bottleneck at any real one.
- **No linearizable reads.** GET always reads local storage immediately,
  even on a follower lagging the leader — see cluster-config.md. A
  `read_index`-style mechanism would fix this without full replication of
  reads.
- **Propose() blocks the calling thread**, including epoll's single event
  loop thread when running in `--io epoll` mode — the same tradeoff
  DurableStorage's fsync already introduced for B5, just with a Raft
  round trip layered on top of the fsync it still does.
- **No RPC timeouts in ClusterTransport.** A hung (not dead) peer's cached
  connection can stall a replication round until the OS's own TCP-level
  failure detection kicks in. A cleanly killed process's socket closes
  immediately, so this hasn't been an issue in testing, but it's not a
  bound guarantee.
- **Fixed 3-way majority arithmetic** — no dynamic membership changes; the
  peer list a node starts with is the peer list it runs with.
