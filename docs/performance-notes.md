# Performance Notes

Profile-driven optimization pass on top of the working, correct Raft
implementation and its failure-testing suite. Every claim below is
backed by a benchmark run recorded under `docs/benchmarks/results/` —
nothing here is asserted without a number next to it.

## Methodology

No sampling profiler (`perf`, etc.) is available in the Docker build
container without elevated privileges it doesn't grant by default, so
bottleneck identification is via targeted comparative benchmarking:
change exactly one variable, hold everything else constant, and read
the delta off real `nkv-bench` runs. See
`docs/benchmarks/results/optimization-baseline-2026-08-24.txt` for the
full raw numbers behind the two findings below.

## Bottleneck 1: WAL append+fsync+apply is fully serialized

`DurableStorage` holds one `write_mutex_` around the entire
append-fsync-apply sequence for every SET/DELETE. Measured effect: a
pure-SET workload's aggregate throughput does **not** increase with
client concurrency — 3547 ops/sec at 1 client, 3143 at 8, 2485 at 32 —
while p50 latency jumps 8x the moment there's more than one concurrent
writer (233us -> 1869us) and stays flat from there. That's the exact
signature of every writer queuing behind a lock that can only do one
`fsync` at a time: more concurrent clients buys more waiting, not more
throughput.

## Bottleneck 2: every GET on a Raft leader pays a full quorum round trip

`RequestHandler`'s GET path calls `RaftNode::ConfirmLeadershipQuorum()`
before every single read, which sends an empty `AppendEntries` to every
peer and waits for a majority reply — a real network round trip, even
though the workload has zero writes. Measured effect: the identical
pure-GET workload runs at 198,751 ops/sec / p50=27us on a single node,
and 12,102 ops/sec / p50=248us against a 3-node Raft leader — a 16.4x
throughput drop and 9.2x p50 increase for work that touches no disk and
replicates nothing. The leader already knows it held the term a moment
ago; re-proving that on every read is avoidable.

## Optimizations applied

### Group commit — fixes Bottleneck 1

Batches concurrent WAL appends into one `fsync` instead of one per
write, bounded by either a record-count cap (16) or a latency cap (1ms)
so no caller waits longer for its own durability than the cap allows.
`WalWriter` gained its own internal locking so `Append`/`Sync` are safe
to call concurrently — `DurableStorage::Set`/`Delete` no longer hold one
lock across the whole append-fsync-apply sequence, which is what
unlocks the actual batching opportunity.

Result: 3.01x throughput on the write-heavy single-node benchmark, and
the write-path scaling check flips from flat-to-declining (more clients,
same or less throughput — the serialization signature) to actually
scaling with concurrency (3.4x–3.65x at 8–32 clients). One honest
regression: the fully uncontended, single-client case gets ~55% slower
(233us -> 364us p50) — the standard group-commit trade-off of a small
bounded tax when there's nothing to batch with, in exchange for a large
win the moment there's real concurrent load. See
`docs/benchmarks/results/group-commit-2026-08-24.txt` for the full
numbers.

Two things had to be solved carefully to get this right, both caught by
the existing test suite (not shipped and then discovered):

- **Apply order.** With one lock gone, two threads' appends and fsyncs
  can now complete out of order relative to each other. The WAL's
  on-disk order must still match the order records get applied to the
  in-memory `ShardedKV` — otherwise a newer write could be overwritten
  by an older one racing to apply second. `DurableStorage` now has a
  small separate apply barrier (a mutex + condition variable, keyed on
  the WAL index) that only orders the *apply* step, not the slow
  append+fsync — so the actual bottleneck being removed doesn't come
  back, but WAL order and apply order stay identical.
- **Don't tax the case with nothing to batch.** The first version of
  the group-commit wait unconditionally slept up to the full 1ms delay
  cap before flushing, regardless of whether anyone else showed up to
  batch with. `RaftNode` calls into its own `WalWriter` while already
  holding its own mutex end-to-end (Raft's per-node log is inherently
  single-threaded — there's never anything to batch there), so that
  version added a flat ~1ms of *pure waste* to every Raft write and
  `AppendEntries` apply, while holding `RaftNode`'s mutex the whole
  time. That was slow enough to change the timing of
  `RaftFailureScenariosTest.LeaderFailoverDuringWrites` (a mid-burst
  leader-kill test) enough to expose a separate, legitimate Raft
  subtlety in that test's own assertion (a freshly elected leader can't
  directly advance `commit_index_` over a *previous* term's entries
  until it commits something in its own term first — §5.4.2 of the
  Raft paper; the test now proposes a flush entry on the new leader
  before checking, matching how a real client would need to retry
  anyway). The wait itself was changed to poll in short "quiet ticks"
  (50us) and flush as soon as one goes by with nothing new arriving,
  rather than always sleeping out the full cap — bounded the same way,
  but no longer pointless for Raft's single-writer-per-node case.

### Read-confirm amortization — fixes Bottleneck 2

`RaftNode` tracks, per peer, the last time it acked *any* AppendEntries
(heartbeat or otherwise) in the current term (`last_ack_time_`, cleared
on every leadership/term change so nothing stale can survive across
one). A GET's `ConfirmLeadershipQuorum()` call skips its own round trip
entirely if a majority of peers already acked within the last heartbeat
interval (75ms) — otherwise it falls back to the full round, exactly as
before. Linearizability is preserved: the guarantee a fresh quorum round
provides — "no other leader has been elected since this moment" — is
exactly as true one heartbeat interval after the leader's own heartbeats
last got majority acks, since a superseding leader would have needed to
win an election in that same window, which itself requires a round of
vote-granting that would have reached this node and demoted it before
it could keep acting as leader.

Isolated effect (read-only workload, amortization on vs. off with group
commit already present in both): 12.2x throughput, p50 5.6x lower, p95
25.1x lower — a Raft-leader GET now costs roughly what a single-node GET
costs. On the standard 80/20 mix: 1.53x throughput, p50 20.6x lower;
p95/p99 got slightly worse, because with GETs no longer sharing (and
smoothing) the tail, the tail now falls entirely on the write share's
own replication latency, which amortization doesn't touch — a real,
disclosed trade-off, not a regression in what actually matters for this
workload (median latency and overall throughput). See
`docs/benchmarks/results/read-confirm-amortization-2026-08-24.txt` for
the full numbers and how the "before" run was isolated.

### Connection reuse in ClusterTransport

See "Rejected / deferred" below.

## Rejected / deferred

- **Per-RPC TCP connect reuse.** Not implemented. Once read-confirm
  amortization lands, the dominant per-GET cost it was meant to address
  (a full round trip on every read) mostly disappears — a confirmed
  leader skips the RPC round entirely rather than just skipping its
  connect overhead. What's left is the ordinary heartbeat traffic every
  75ms, which already pays one connect per peer per heartbeat regardless
  of reuse; reusing a persistent connection here reintroduces the exact
  BlockingServer connection-starvation bug fixed earlier in the project
  (a held-open connection under continuous heartbeat traffic monopolizes
  a single-threaded server's one serving slot) unless paired with
  non-trivial safeguards not currently needed. Revisit if a future
  profile shows heartbeat connect overhead is itself significant once
  read-path savings are accounted for.
- **Snapshots, multi-Raft sharding, io_uring** — out of scope here; see
  raft-design.md for snapshots specifically (still deferred, unrelated
  to this optimization work).
