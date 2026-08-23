# Cluster Configuration

Static cluster membership for a multi-node NeuralKV deployment. Leadership
itself is elected by Raft — see [raft-design.md](raft-design.md) for how —
not read from this file.

## Config file format

```
node_id=1
peer 1 127.0.0.1 7401
peer 2 127.0.0.1 7402
peer 3 127.0.0.1 7403
```

- `node_id=<n>` — this node's own id. Must match `--node-id` on the
  command line; `nkv-server` refuses to start otherwise.
- `peer <id> <host> <port>` — one line per node in the cluster,
  **including the local node**. Every node's config lists the same
  peers; only `node_id` differs between them, since the file bakes in
  "who am I" as well as "who else is out there".
- `leader_id=<n>` is accepted for backward compatibility with configs
  written before Raft landed, but is otherwise ignored: Raft owns
  leadership now, not a config file.
- Blank lines and lines starting with `#` are ignored.

Loading fails if the local `node_id` isn't in the peer list, a present
`leader_id` isn't in the peer list, or any peer id repeats.

## Running a node with cluster config

```
nkv-server --node-id 1 --cluster-config cluster.conf --port 7401 --data-dir ./data/node-1
```

`--node-id` is required whenever `--cluster-config` is set. Without
`--cluster-config`, a node runs exactly as a single-node server always
has — no RaftNode is constructed, so every write applies directly to
storage regardless of any notion of leadership.

`scripts/run_cluster.sh` starts a 3-node cluster on localhost (ports
7401–7403) with generated per-node config files, for manual testing.

## Leader semantics

- **SET / DELETE**: accepted only on the node Raft currently elected as
  leader. Any other node rejects the request with
  `ResponseStatus::kWrongLeader` and `leader_hint` set to the current
  leader's node id (0 if no leader is known yet, e.g. mid-election) —
  without touching its own storage, so a rejection has no side effects to
  undo. A write can also come back `kWrongLeader` if this node *was* the
  leader when the request arrived but lost the role before the write
  committed; the client should just retry.
- **GET**: always served from local storage, on every node, leader or
  follower. A follower's log can lag the leader's by however long
  replication takes to catch up — a stale read, not a redirect.
  Redirecting GET to the leader for a linearizable read is future work.

## Client redirect

`nkv-client` optionally takes `--cluster-config <path>`. On a
`kWrongLeader` response with a non-zero `leader_hint`:

- **Without** `--cluster-config`: prints a note to stderr naming the
  leader's node id and still exits with the `WRONG_LEADER` failure —
  nothing is retried.
- **With** `--cluster-config`: looks up `leader_hint` in the config's
  peer list, prints `redirected to leader node <id> at <host>:<port>`,
  and retries the same request against that address exactly once. A
  second `kWrongLeader` (stale config, mid-failover) is not retried
  again — that's a real error, not a client-side masking exercise.

## Internal cluster RPC

Node-to-node RPCs share the same TCP port and framing as the client
protocol, distinguished by `MessageType` (`kClusterRequest` /
`kClusterResponse` vs. `kClientRequest` / `kClientResponse`).
`Connection::ProcessFrames` and `ServeClientSession` both dispatch on
this before decoding, so every server mode (`blocking`, `threadpool`,
`epoll`) answers cluster RPCs without a second listener.

Cluster RPCs today: `ClusterOpcode::kPing` (liveness — gets back
`ResponseStatus::kOk` and body `"pong"`), and Raft's own
`kRequestVote`/`kAppendEntries`, whose bodies are encoded/decoded by
`raft::rpc_codec` and dispatched to the node's `RaftNode`.
`ClusterTransport` dials and caches one connection per peer and reuses it
across calls, closing and discarding it on any malformed response or I/O
error so the next call reconnects rather than reusing a socket left in a
bad state.

## Out of scope here

Dynamic membership changes and TLS are deliberately not part of this
config format. See raft-design.md for what's out of scope in Raft itself
(snapshots, linearizable reads).
