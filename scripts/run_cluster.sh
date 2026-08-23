#!/usr/bin/env bash
set -euo pipefail

# Starts a 3-node NeuralKV cluster on localhost: node 1 is the static
# leader (writes to nodes 2/3 get rejected with WRONG_LEADER). Each node
# gets its own generated config file — same peer list and leader_id, only
# node_id differs — since the wire format bakes a node's own id into the
# file it loads.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${NKV_BUILD_DIR:-$repo_root/build}"
server_bin="$build_dir/tools/nkv-server/nkv-server"

if [[ ! -x "$server_bin" ]]; then
  echo "nkv-server binary not found at $server_bin" >&2
  echo "build it first (cmake --build $build_dir) or set NKV_BUILD_DIR" >&2
  exit 1
fi

cluster_dir="${NKV_CLUSTER_DIR:-/tmp/nkv-cluster}"
rm -rf "$cluster_dir"
mkdir -p "$cluster_dir"

pids=()
for node_id in 1 2 3; do
  port=$((7400 + node_id))
  data_dir="$cluster_dir/node-$node_id"
  conf="$cluster_dir/cluster-$node_id.conf"
  log="$cluster_dir/node-$node_id.log"
  mkdir -p "$data_dir"

  cat > "$conf" <<EOF
node_id=$node_id
leader_id=1
peer 1 127.0.0.1 7401
peer 2 127.0.0.1 7402
peer 3 127.0.0.1 7403
EOF

  "$server_bin" --node-id "$node_id" --cluster-config "$conf" --port "$port" \
      --data-dir "$data_dir" > "$log" 2>&1 &
  pid=$!
  pids+=("$pid")
  echo "node $node_id: pid $pid, port $port, log $log"
done

printf '%s\n' "${pids[@]}" > "$cluster_dir/pids"
sleep 0.5

echo
echo "leader: node 1 (127.0.0.1:7401)"
echo "try:    nkv-client --port 7401 set foo bar   # leader accepts writes"
echo "        nkv-client --port 7402 set foo bar   # follower: WRONG_LEADER"
echo
echo "stop with: kill ${pids[*]}"
echo "or:        kill \$(cat $cluster_dir/pids)"
