# NeuralKV

NeuralKV is a distributed, in-memory key-value store built from scratch in
C++20: custom binary TCP protocol, epoll-driven networking, write-ahead-log
durability, and Raft-based replication across a 3-node cluster.

## Prerequisites

- CMake 3.20+
- A C++20 compiler (Apple Clang, GCC, or Clang)
- Docker (optional, for Linux builds from macOS)

## Native Build (macOS / Linux)

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the benchmark sanity check and the load generator's help text:

```sh
./build/benchmarks/noop_bench
./build/tools/nkv-bench/nkv-bench --help
```

## Docker Build (Linux target)

The deployment target is Linux; this runs the same build and test suite
inside an Ubuntu container:

```sh
./scripts/docker_build.sh
```

## Benchmarking

Start a server, then point nkv-bench at it: `nkv-server --port 7400 &` then
`nkv-bench --bench --host 127.0.0.1 --port 7400 --duration 30 --clients 16`.

To compare the blocking baseline (B1) against the thread-pool server (B2),
run the same nkv-bench command against each: `nkv-server --port 7400`
(single-threaded) vs. `nkv-server --port 7400 --workers 8` (thread pool),
then diff the throughput and percentiles nkv-bench prints for each run.

B4 (epoll event loop) is Linux-only: `nkv-server --io epoll --port 7400`
under Docker or on a Linux host. Compare it against B2 the same way — same
nkv-bench command, `--workers 8` vs. `--io epoll` — and diff the printed
throughput and percentiles. No numbers are published here; run it and read
what your machine reports.

B5 (write-ahead log) is on unconditionally as of this change — every SET
and DELETE appends to the WAL and `fsync`s before it's applied, regardless
of `--io` mode. To see the fsync cost, run the same nkv-bench command
against a server from before this change and one after; the gap between
them is B5's durability tax on top of B4. Because every write serializes
through one `fsync` (`DurableStorage` holds a single lock around
append+sync+apply), throughput does not scale with `--workers` or
connection count the way B2/B4's read-heavy numbers did — use
`--label b5-wal` to tag the run.

B6 (3-node Raft) measures consensus overhead on top of B5: point
nkv-bench at the elected leader of a Raft cluster (see below) instead of
a single-node server, same command otherwise. Every SET now replicates
to a majority before the client sees a response, so expect materially
lower throughput and higher tail latency than a single node — see
[docs/benchmarks/results/b6-raft-2026-08-23.txt](docs/benchmarks/results/b6-raft-2026-08-23.txt)
for real numbers.

B9 measures failover time instead of throughput: `SIGKILL` a running
cluster's leader and time how long until a surviving node accepts a
write. Median 289ms across 3 runs on Linux/Docker, comfortably inside
Raft's randomized 250–400ms election timeout — see
[docs/benchmarks/results/b9-failover-2026-08-24.txt](docs/benchmarks/results/b9-failover-2026-08-24.txt).

B11 is a profile-driven optimization pass on top of B5/B6/B9: measuring
found two real bottlenecks (the WAL's single fsync-per-write serializing
every writer, and every Raft-leader GET paying a full quorum round trip)
and fixed both. WAL group commit took single-node write-heavy throughput
up 3.01x and turned the write path from "more concurrent clients doesn't
help" into "actually scales with concurrency" (3.4x–3.65x at 8–32
clients). Read-confirm amortization took Raft-leader read throughput up
12.2x on a read-only workload and the standard 80/20 mix up 1.53x, with
p50 down 20.6x, without weakening linearizability. See
[docs/performance-notes.md](docs/performance-notes.md) for the
methodology, what was measured, what was fixed, and one optimization
(connection reuse) that was evaluated and deliberately not implemented,
plus [docs/benchmarks/results](docs/benchmarks/results/) for every
before/after number (`b11-*`).

## Cluster (Raft-replicated)

`./scripts/run_cluster.sh` starts a 3-node cluster on localhost. Raft
elects the leader; writes against any other node come back as
`WRONG_LEADER` with the current leader's node id, and `nkv-client
--cluster-config <path>` follows that redirect automatically. Reads are
linearizable by default too: a follower's GET also comes back
`WRONG_LEADER` (nothing local to serve with a currency guarantee), and
the leader confirms it still holds a live quorum before answering —
`--allow-stale-reads` opts back into serving GET from local storage
unconditionally, at the cost of the currency guarantee. Killing the
leader triggers a new election. See [docs/raft-design.md](docs/raft-design.md)
for how election, replication, commit/apply, and linearizable reads work,
and [docs/cluster-config.md](docs/cluster-config.md) for the config file
format and client redirect behavior.

## Failure testing

`./scripts/run_fault_tests.sh` builds if needed and runs just the
fault-injection and failure-scenario test suites (leader crash, network
partition, delayed RPCs, concurrent clients across a leader change,
single-key linearizability under load) instead of the full suite — see
[docs/failure-testing.md](docs/failure-testing.md) for the failure model
this covers, what's in scope (in-process fault injection via
`src/testing/fault_injection.h`) and what isn't (no `iptables`/network
namespaces, no scripted fault schedules, no full Jepsen-style checker).

## Project Layout

```
src/common/       core types shared across the project (Status, Result<T>, NodeConfig)
src/storage/      sharded, thread-safe in-memory key-value store (ShardedKV)
src/protocol/     binary wire protocol: frame codec, request/response types
src/net/          POSIX socket RAII, blocking I/O helpers, connection state
                  machine, epoll event loop (Linux only)
src/persistence/  write-ahead log, crash recovery, durable storage engine
src/cluster/      cluster config, node-to-node RPC transport
src/raft/         Raft consensus: election, log replication, commit/apply
src/server/       request handler, blocking and thread-pool TCP servers
src/testing/      test-only fault injection and a linearizability checker;
                  never linked into nkv-server
tests/unit/       GoogleTest unit tests
tests/integration/ end-to-end tests against a forked nkv-server subprocess
tests/raft/       in-process Raft tests (real RaftNode/BlockingServer
                  objects, no subprocess)
benchmarks/       Google Benchmark micro-benchmarks
tools/nkv-server/ standalone server binary
tools/nkv-client/ one-shot CLI client (set/get/delete)
tools/nkv-bench/  load generator and latency benchmark
docker/           Linux container build definition
scripts/          helper scripts (Docker build/test, run_cluster.sh)
docs/             design docs and benchmark methodology
```

## Platform Notes

Linux is the deployment target; macOS is supported for local development.
Code that depends on Linux-only APIs (epoll, etc.) is gated behind the
`NEURALKV_LINUX` compile definition, set automatically when building on
Linux.
