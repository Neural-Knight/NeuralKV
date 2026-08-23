# Cluster Configuration

Static cluster membership and a fixed leader designation for a multi-node
NeuralKV deployment. There is no election and no log replication yet —
this is networking and configuration groundwork for Raft (M8). Until
then, "leader" means one specific node named in a config file, not one
elected by consensus.

## Config file format

```
node_id=1
leader_id=1
peer 1 127.0.0.1 7401
peer 2 127.0.0.1 7402
peer 3 127.0.0.1 7403
```

- `node_id=<n>` — this node's own id. Must match `--node-id` on the
  command line; `nkv-server` refuses to start otherwise.
- `leader_id=<n>` — the id of the node currently treated as leader.
- `peer <id> <host> <port>` — one line per node in the cluster,
  **including the local node**. Every node's config lists the same
  peers and the same `leader_id`; only `node_id` differs between them,
  since the file bakes in "who am I" as well as "who else is out there".
- Blank lines and lines starting with `#` are ignored.

Loading fails if the local `node_id` isn't in the peer list, `leader_id`
isn't in the peer list, or any peer id repeats.

## Running a node with cluster config

```
nkv-server --node-id 1 --cluster-config cluster.conf --port 7401 --data-dir ./data/node-1
```

`--node-id` is required whenever `--cluster-config` is set. Without
`--cluster-config`, a node runs exactly as a single-node server always
has — `RequestHandler` has no cluster config to consult, so every write
applies locally regardless of any notion of leadership.

`scripts/run_cluster.sh` starts a 3-node cluster on localhost (node 1 as
leader, ports 7401–7403) with generated per-node config files, for manual
testing.

## Leader semantics

- **SET / DELETE**: applied locally only on the node whose `node_id`
  equals `leader_id`. A follower rejects the request with
  `ResponseStatus::kWrongLeader` and `leader_hint` set to the leader's
  node id — without touching its WAL or storage at all, so a follower's
  rejection has no side effects to undo.
- **GET**: always served from local storage, on every node, leader or
  follower. There is no replication in this milestone, so a follower's
  data is whatever was last written directly to it (nothing, unless
  something wrote to it before this milestone, or it was promoted to
  leader in a different config in the past) — a permanently stale read,
  not a "slightly behind" one. Redirecting GET to the leader is left for
  when replication exists and staleness is actually bounded.

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

The only RPC today is a liveness check: `ClusterOpcode::kPing` gets back
a response with `ResponseStatus::kOk` and body `"pong"`. `ClusterTransport`
dials and caches one connection per peer and reuses it across calls,
closing and discarding it on any malformed response or I/O error so the
next call reconnects rather than reusing a socket left in a bad state.

## Out of scope here

Raft leader election, log replication, `AppendEntries` with real
entries, dynamic membership changes, and TLS are all deliberately not
part of this milestone.
