# Benchmark Methodology

This document defines how NeuralKV is benchmarked so results are
reproducible and comparable across architectural stages. Fill in the
template fields with real measurements — never publish a number without
recording how it was produced.

## Hardware Baseline

| Field | Value |
|---|---|
| CPU model | _fill in_ |
| Physical cores / threads | _fill in_ |
| RAM | _fill in_ |
| Disk | _fill in_ (NVMe / SSD / HDD, model) |
| Network (multi-node runs) | _fill in_ |
| OS | _fill in_ |
| Kernel version | _fill in_ (Linux only, where relevant) |

## Software Versions

| Field | Value |
|---|---|
| Compiler | _fill in_ |
| Build type | _fill in_ (expect Release, `-O3`) |
| CMake version | _fill in_ |
| NeuralKV commit | _fill in_ |

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
measured in isolation. Results are filled in as each stage is implemented —
no placeholder numbers below.

| Stage | Architecture | Purpose | Result |
|---|---|---|---|
| B0 | In-memory map, no I/O | Storage-layer throughput ceiling | |
| B1 | Blocking TCP, single thread | Syscall-bound baseline | |
| B2 | Thread-pool TCP | Concurrency vs. B1 | |
| B3 | Sharded storage | Lock-contention reduction vs. B2 | |
| B4 | Epoll event loop | Event-driven I/O vs. thread pool | |
| B5 | + WAL fsync | Durability cost on top of B4 | |
| B6 | 3-node Raft | Consensus overhead vs. single-node B5 | |

## Reproducibility

Each result set must ship with:

- The exact command used to build (`cmake` invocation and flags).
- The exact command used to run the benchmark, including all flags.
- The hardware and software tables above, filled in for that run.
- Raw output or CSV, not just a computed summary.

Anyone with the same hardware class should be able to reproduce a result
within measurement noise using only what's recorded here.
