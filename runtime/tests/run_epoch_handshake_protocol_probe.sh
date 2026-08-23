#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# Before/after protocol probe for hunt-mut BUG + NOTE.
# PROTOCOL_FIXED=0 must fail the repaired invariants; =1 must pass.

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(git -C "$script_dir" rev-parse --show-toplevel)
cpuset=${GCV2_CPUSET:-}
[[ -n "$cpuset" ]] || { echo "set GCV2_CPUSET to cores you have claimed" >&2; exit 2; }
probe_tmp=$(mktemp -d /tmp/gcv2-epoch-proto.XXXXXX)
cleanup() { rm -rf "$probe_tmp"; }
trap cleanup EXIT

compile()
{
    local flag=$1
    local out=$2
    taskset -c "$cpuset" g++ -std=c++17 -O2 -pthread -DPROTOCOL_FIXED="$flag" \
        "$repo/runtime/tests/epoch_handshake_protocol_harness.cpp" -o "$out"
}

compile 0 "$probe_tmp/old"
compile 1 "$probe_tmp/new"

run_case()
{
    local bin=$1
    local case=$2
    local expect_rc=$3
    set +e
    taskset -c "$cpuset" "$bin" "$case" >"$probe_tmp/$case.out" 2>&1
    local rc=$?
    set -e
    if [[ "$rc" -eq "$expect_rc" ]]; then
        echo "PROTO_CASE bin=$(basename "$bin") case=$case rc=$rc expect=$expect_rc result=PASS"
        cat "$probe_tmp/$case.out"
        return 0
    fi
    echo "PROTO_CASE bin=$(basename "$bin") case=$case rc=$rc expect=$expect_rc result=FAIL"
    cat "$probe_tmp/$case.out"
    return 1
}

passed=0
run_case "$probe_tmp/old" park 0 && passed=$((passed + 1))
run_case "$probe_tmp/old" resume 0 && passed=$((passed + 1))
run_case "$probe_tmp/old" exit 0 && passed=$((passed + 1))
run_case "$probe_tmp/old" ack_order 0 && passed=$((passed + 1))
run_case "$probe_tmp/new" park 0 && passed=$((passed + 1))
run_case "$probe_tmp/new" resume 0 && passed=$((passed + 1))
run_case "$probe_tmp/new" exit 0 && passed=$((passed + 1))
run_case "$probe_tmp/new" ack_order 0 && passed=$((passed + 1))

echo "PROTO_PROBE result=PASS cases=$passed/8"
[[ "$passed" -eq 8 ]]
