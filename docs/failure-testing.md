# Failure Testing

How NeuralKV's Raft implementation is tested under injected faults —
node crashes, network partitions, delayed RPCs — beyond the happy-path
election/replication tests in `tests/raft/raft_figure8_test.cpp`.

## Scope

Fault injection here is **in-process**, not `iptables` or network
namespaces: `src/testing/fault_injection.h`'s `FaultInjectingTransport`
wraps `cluster::ClusterTransport::SendRpc` and can drop, delay, or
probabilistically drop outbound RPCs to specific peers before they'd
otherwise be sent, plus a `FaultInjectionController` that coordinates a
partition between two specific node ids across their two transports.
This is enough to test how `RaftNode` itself reacts to a fault and to a
fault healing — it is not a systematic exploration of every possible
fault schedule, message reordering, or timing interleaving. A real
Jepsen/Elle-style harness is a different, much larger project; not
attempted here.

Two harness styles are used, matching what each test actually needs:

- **In-process** (`tests/raft/raft_failure_scenarios_test.cpp`,
  `raft_fault_injection_test.cpp`, `raft_linearizability_test.cpp`):
  real `DurableStorage` + `RaftNode` + `BlockingServer` objects
  constructed directly in the test process, wired with
  `FaultInjectingTransport` where the scenario needs one. Fast, no
  fork/exec overhead, and gives the test direct access to
  `RaftNode::replication_lag_entries()` and friends for precise
  assertions.
- **Subprocess** (`tests/integration/raft_failure_integration_test.cpp`):
  real forked `nkv-server` processes, talked to over real TCP with the
  real wire protocol — closer to how a deployed cluster actually
  behaves, at the cost of fork/exec/connect overhead in the timing
  budgets.

## Failure model

| Scenario | Expected behavior | Test covering it |
|---|---|---|
| Leader crash | New leader elected within its randomized election timeout; every committed write survives | `RaftFigure8Test.SurvivesLeaderCrashWithoutLosingCommittedEntries`, `RaftFailureScenariosTest.LeaderFailoverDuringWrites`, `RaftFailureIntegrationTest.LeaderKillUnderLoad` |
| Follower crash | Cluster continues serving writes on the remaining majority | `RaftClusterTest.NewLeaderElectedAfterLeaderCrash` (crashes the leader, which is the harder case; a follower crash is strictly easier since it never held quorum-granting power) |
| Leader isolated (minority partition) | No new writes are acknowledged while isolated; cluster reconverges once healed | `RaftFailureScenariosTest.MinorityPartitionCannotCommit` |
| Follower isolated, majority intact | Majority side keeps committing; the isolated node can't become a commit source while cut off | `RaftFailureScenariosTest.MajorityPartitionContinues` |
| Partition heals | Follower catches up fully (`replication_lag_entries` reaches 0) | `RaftFailureScenariosTest.RejoinAfterPartition`, `RaftFaultInjectionTest.DelayedFollowerCatchesUpAfterPartitionHeals`, `RaftCatchupTest.FollowerCatchesUpAfterRestart` |
| Delayed RPCs | No spurious commits; the gating logic itself (drop/delay/rate/partition) behaves as configured | `FaultInjectionTest.*` |
| Concurrent clients across a leader change | No client ever reads back a value older than one it already saw confirmed (basic monotonicity, not full linearizability) | `RaftFailureIntegrationTest.ConcurrentClientsSurviveLeaderChange` |
| Single-key linearizability under concurrent SET/GET | Every GET's value is consistent with some real-time-respecting order of the SETs that produced it | `RaftLinearizabilityTest.ThreeClientsSingleKeyFiveHundredOps` (checker: `src/testing/linearizability_checker.h`) |

## Running just these tests

```
./scripts/run_fault_tests.sh
```

Builds if needed, then runs the test suites above by name (not a CTest
`LABEL` — gtest-discovered tests share a binary with unrelated suites,
so a per-binary label would over-tag; a name regex targets exactly
these). Exits non-zero if any of them fail.

## What this doesn't cover

- **Snapshots/InstallSnapshot** — still deferred (see raft-design.md);
  no fault scenario here exercises a compacted log.
- **Dynamic membership** — every scenario runs a fixed 3-node peer list.
- **Systematic/scripted fault schedules** — `FaultInjectingTransport`'s
  state is set up once per scenario and (mostly) held for its duration,
  not varied over time within a single test run.
- **True network-level partitioning** — a partition here means "this
  process's `SendRpc` returns an error for this peer," not anything at
  the socket, kernel, or network layer. It's indistinguishable from a
  real partition from `RaftNode`'s point of view (it only ever sees
  `SendRpc`'s return value), but it doesn't exercise TCP-level failure
  modes like half-open connections or a peer that accepts but never
  responds.
