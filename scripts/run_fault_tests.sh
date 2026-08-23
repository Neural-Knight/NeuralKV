#!/usr/bin/env bash
set -euo pipefail

# Runs just the fault-injection and failure-scenario tests, rather than
# the full suite — useful when iterating on Raft's failure handling
# without waiting on every other test binary.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

build_dir="${NKV_BUILD_DIR:-$repo_root/build}"

if [[ ! -d "$build_dir" ]]; then
  cmake -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "$build_dir" -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# Name-based filtering rather than a CTest LABEL: these test suites map
# 1:1 to the files this covers (raft_fault_injection_test,
# raft_failure_scenarios_test, raft_failure_integration_test,
# raft_linearizability_test), plus the lower-level fault_injection_test
# unit test that all of them build on.
pattern='^(FaultInjectionTest|RaftFaultInjectionTest|RaftFailureScenariosTest|RaftFailureIntegrationTest|RaftLinearizabilityTest)\.'

echo "Running fault-injection and failure-scenario tests"
echo "  pattern: $pattern"
echo

set +e
ctest --test-dir "$build_dir" --output-on-failure -R "$pattern"
exit_code=$?
set -e

echo
if [[ "$exit_code" -eq 0 ]]; then
  echo "fault test suite: PASS"
else
  echo "fault test suite: FAIL"
fi
exit "$exit_code"
