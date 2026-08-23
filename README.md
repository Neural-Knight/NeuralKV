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

## Cluster (Raft-replicated)

`./scripts/run_cluster.sh` starts a 3-node cluster on localhost. Raft
elects the leader; writes against any other node come back as
`WRONG_LEADER` with the current leader's node id, and `nkv-client
--cluster-config <path>` follows that redirect automatically. Killing the
leader triggers a new election — see [docs/raft-design.md](docs/raft-design.md)
for how election, replication, and commit/apply work, and
[docs/cluster-config.md](docs/cluster-config.md) for the config file
format and client redirect behavior.

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
tests/unit/       GoogleTest unit tests
tests/integration/ end-to-end tests against a forked nkv-server subprocess
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
