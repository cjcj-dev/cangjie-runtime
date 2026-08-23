#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# TSAN probe for CleanThreadLocalData's shared first-init flag.

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(git -C "$script_dir" rev-parse --show-toplevel)
cpuset=${GCV2_CPUSET:-}
[[ -n "$cpuset" ]] || { echo "set GCV2_CPUSET to cores you have claimed" >&2; exit 2; }
probe_tmp=$(mktemp -d /tmp/gcv2-tls-init.XXXXXX)
cleanup() { rm -rf "$probe_tmp"; }
trap cleanup EXIT

compile()
{
    local flag=$1
    local out=$2
    taskset -c "$cpuset" g++ -std=c++17 -O1 -g -fsanitize=thread -pthread \
        -DPROTOCOL_FIXED="$flag" \
        "$repo/runtime/tests/threadlocal_init_race_harness.cpp" -o "$out"
}

compile 0 "$probe_tmp/old"
compile 1 "$probe_tmp/new"

run_tsan()
{
    local bin=$1
    local expect_race=$2
    set +e
    TSAN_OPTIONS="halt_on_error=0 report_bugs=1" \
        taskset -c "$cpuset" "$bin" >"$probe_tmp/$(basename "$bin").out" 2>&1
    local rc=$?
    set -e
    local races
    races=$(grep -c 'WARNING: ThreadSanitizer: data race' "$probe_tmp/$(basename "$bin").out" || true)
    echo "TLS_CASE bin=$(basename "$bin") rc=$rc races=$races expect_race=$expect_race"
    if [[ "$expect_race" -eq 1 && "$races" -eq 0 ]]; then
        echo "TLS_CASE result=FAIL missing TSAN race"
        cat "$probe_tmp/$(basename "$bin").out"
        return 1
    fi
    if [[ "$expect_race" -eq 0 && "$races" -ne 0 ]]; then
        echo "TLS_CASE result=FAIL unexpected TSAN race"
        cat "$probe_tmp/$(basename "$bin").out"
        return 1
    fi
    echo "TLS_CASE result=PASS"
    return 0
}

passed=0
run_tsan "$probe_tmp/old" 1 && passed=$((passed + 1))
run_tsan "$probe_tmp/new" 0 && passed=$((passed + 1))
echo "TLS_PROBE result=PASS cases=$passed/2"
[[ "$passed" -eq 2 ]]
