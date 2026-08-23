# NeuralKV

NeuralKV is a distributed, in-memory key-value store built from scratch in
C++20: custom binary TCP protocol, epoll-driven networking, write-ahead-log
durability, and Raft-based replication across a 3-node cluster. This
repository currently holds the project foundation — build system, core
types, and test/benchmark infrastructure — with the networking and
consensus layers built on top of it incrementally.

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

Run the benchmark sanity check and the load-generator CLI skeleton:

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

## Project Layout

```
src/common/       core types shared across the project (Status, Result<T>, NodeConfig)
src/storage/      sharded, thread-safe in-memory key-value store (ShardedKV)
src/protocol/     binary wire protocol: frame codec, request/response types
src/net/          POSIX socket RAII and blocking read/write helpers
src/server/       request handler and single-threaded blocking TCP server
tests/unit/       GoogleTest unit tests
tests/integration/ end-to-end tests against a forked nkv-server subprocess
benchmarks/       Google Benchmark micro-benchmarks
tools/nkv-server/ standalone server binary
tools/nkv-client/ one-shot CLI client (set/get/delete)
tools/nkv-bench/  load generator and latency benchmark
docker/           Linux container build definition
scripts/          helper scripts (Docker build/test)
docs/             design docs and benchmark methodology
```

## Platform Notes

Linux is the deployment target; macOS is supported for local development.
Code that depends on Linux-only APIs (epoll, etc.) is gated behind the
`NEURALKV_LINUX` compile definition, set automatically when building on
Linux.
