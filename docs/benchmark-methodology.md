# Benchmark Methodology

This document defines how NeuralKV is benchmarked so results are
reproducible and comparable across architectural stages. Fill in the
template fields with real measurements — never publish a number without
recording how it was produced.

## Hardware Baseline

Two environments are tracked. Numbers below are dev-machine sanity runs;
Linux target-hardware fields stay blank until run on real deployment
hardware.

| Field | Dev machine (macOS) | Linux target |
|---|---|---|
| CPU model | Apple M5 Pro | _fill in_ |
| Physical cores / threads | 18 / 18 | _fill in_ |
| RAM | 48 GB | _fill in_ |
| Disk | SSD (internal) | _fill in_ (NVMe / SSD / HDD, model) |
| Network (multi-node runs) | n/a (single node) | _fill in_ |
| OS | macOS 26.5.1 | _fill in_ |
| Kernel version | Darwin 25.5.0 | _fill in_ |

## Software Versions

| Field | Value |
|---|---|
| Compiler | Apple clang 21.0.0 |
| Build type | Release, `-O3` |
| CMake version | 4.4.2 |
| NeuralKV commit | 60f6892 (plus uncommitted changes at run time) |

## Workload Definition

| Parameter | Default | Notes |
|---|---|---|
| Key size | 16 B | fixed |
| Value size | 256 B | also sweep 64 B and 1 KB |
| Key space | 1,000,000 keys | uniform random distribution |
| Operation mix | 80% GET / 20% SET | configurable per run |
| Client concurrency | 1, 4, 16, 64 | swept, reported per point |

## Metrics

- Throughput (ops/sec)
- Latency: p50, p95, p99, p999
- CPU utilization during the run
- Peak resident memory (RSS)
- Failover time (leader kill to first successful write), where applicable
- Recovery time (node restart to fully caught up), where applicable

## Procedure

1. Warm up for 10 seconds; discard warmup samples.
2. Run for 60 seconds per data point.
3. Repeat each configuration 3 times; report the median run.
4. Record hardware and software tables above for every result set.
5. Pin client and server to fixed CPU cores where the environment allows it,
   and note whether pinning was used.

## Stages

Each stage isolates one architectural change so its cost or benefit can be
measured in isolation. Results below are dev-machine sanity runs (see
Hardware Baseline) — single run each, not the 3-run median at full 60s
duration the Procedure section calls for. Treat them as directional until
rerun on target hardware following the full procedure.

| Stage | Architecture | Purpose | Result |
|---|---|---|---|
| B0 | In-memory map, no I/O | Storage-layer throughput ceiling | Set ~4.9M/s, Get ~4.5M/s, mixed 80/20 ~5.0M/s (single-threaded); sharded-lock concurrency: 1 thread 32.9M/s → 4 threads 8.0M/s → 8 threads 4.6M/s → 16 threads 2.0M/s. Raw: [b0-storage-2026-08-23.csv](benchmarks/results/b0-storage-2026-08-23.csv) |
| B1 | Blocking TCP, single thread | Syscall-bound baseline | 1 client, 20s, 80/20 mix: 72,746 ops/sec, p50=13µs p95=18µs p99=22µs. Raw: [b1-blocking-tcp-2026-08-23.txt](benchmarks/results/b1-blocking-tcp-2026-08-23.txt) |
| B2 | Thread-pool TCP | Concurrency vs. B1 | |
| B3 | Sharded storage | Lock-contention reduction vs. B2 | |
| B4 | Epoll event loop | Event-driven I/O vs. thread pool | |
| B5 | + WAL fsync | Durability cost on top of B4 | Linux/Docker, 30s, 80/20 mix, WAL always on: epoll (1 thread) 8 clients 10,135 ops/sec (p50=52µs p95=3412µs p99=4118µs); thread-pool (8 workers) 8 clients 14,263 ops/sec (p50=30µs p95=3094µs p99=3826µs); thread-pool (64 workers) 64 clients 9,145 ops/sec (p50=35µs p95=18,273µs p99=21,416µs) — write throughput is capped by one fsync at a time regardless of thread count (DurableStorage serializes the whole log with one mutex), so more workers mainly buys headroom for concurrent GETs, not more concurrent SETs; tail latency grows with contention on that same lock. Raw: [b5-wal-2026-08-23.txt](benchmarks/results/b5-wal-2026-08-23.txt) |
| B6 | 3-node Raft | Consensus overhead vs. single-node B5 | Linux/Docker, 30s, 80/20 mix, threadpool (8 workers), 8 clients, writes against the elected leader: single-node baseline 18,481 ops/sec (p50=31µs p95=1948µs p99=2132µs) vs. 3-node cluster 9,141.6 ops/sec (p50=35µs p95=4045µs p99=5873µs) — ~2.0x lower throughput; p50 barely moves since the 80% GET share never touches Raft, but p95/p99 roughly double-to-triple from the 20% SET share now paying for majority-commit AppendEntries round trips to both followers. Raw: [b6-raft-2026-08-23.txt](benchmarks/results/b6-raft-2026-08-23.txt) |
| B9 | 3-node Raft, leader killed | Failover time: leader `SIGKILL` to first successful write on the new leader | Linux/Docker, idle cluster (no concurrent write load), 3 fresh-cluster runs: 382ms, 275ms, 289ms — **median 289ms**, comfortably inside the randomized 250–400ms election timeout window (raft/node.cpp). Raw: [b9-failover-2026-08-24.txt](benchmarks/results/b9-failover-2026-08-24.txt) |

## Reproducibility

Each result set must ship with:

- The exact command used to build (`cmake` invocation and flags).
- The exact command used to run the benchmark, including all flags.
- The hardware and software tables above, filled in for that run.
- Raw output or CSV, not just a computed summary.

Anyone with the same hardware class should be able to reproduce a result
within measurement noise using only what's recorded here.
