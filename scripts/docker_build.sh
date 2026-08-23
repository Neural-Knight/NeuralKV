#!/usr/bin/env bash
set -euo pipefail

if ! command -v docker >/dev/null 2>&1; then
  echo "docker not found on PATH; install Docker to run the Linux build" >&2
  exit 1
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

docker compose -f docker/dev.yml build

set +e
docker compose -f docker/dev.yml run --rm build
exit_code=$?
set -e

docker compose -f docker/dev.yml down --remove-orphans >/dev/null 2>&1 || true

exit "$exit_code"
