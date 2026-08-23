#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(git -C "$script_dir" rev-parse --show-toplevel)
cpuset=${GCV2_CPUSET:-}
# fail-closed: a hard-coded default used to taskset onto cores another lane had
# claimed (0-63 / 96-127 / 112-127 are all inside the reservable range).
[[ -n "$cpuset" ]] || { echo "set GCV2_CPUSET to cores you have claimed" >&2; exit 2; }
runtime_lib_dir=${GCV2_RUNTIME_LIB_DIR:-$repo/runtime/output/temp/lib/x86_64_Relwithdebinfo}
probe_tmp=$(mktemp -d /tmp/gcv2-sizeguard-probe.XXXXXX)

cleanup()
{
    unlink "$probe_tmp/object-size-guard" 2>/dev/null || true
    rm -f "$probe_tmp"/*.stdout "$probe_tmp"/*.stderr
    rmdir "$probe_tmp"
}
trap cleanup EXIT

taskset -c "$cpuset" clang++ -std=gnu++14 -O2 -pthread -fno-rtti -fno-exceptions \
    -I"$repo/runtime/src" -I"$repo/runtime/src/Heap" -I"$repo/runtime/include" \
    -I"$repo/runtime/output/temp/include" \
    -I"$repo/runtime/third_party/third_party_bounds_checking_function/include" \
    "$repo/runtime/tests/object_size_guard_harness.cpp" \
    -L"$runtime_lib_dir" -Wl,-rpath,"$runtime_lib_dir" -lcangjie-runtime -lboundscheck \
    -o "$probe_tmp/object-size-guard"

passed=0
for test_case in zero unaligned oversize; do
    set +e
    LD_LIBRARY_PATH="$runtime_lib_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        taskset -c "$cpuset" "$probe_tmp/object-size-guard" "$test_case" \
        >"$probe_tmp/$test_case.stdout" 2>"$probe_tmp/$test_case.stderr"
    rc=$?
    set -e
    cat "$probe_tmp/$test_case.stderr"
    if [[ "$rc" -eq 134 ]] &&
        grep -aq '\[GCV2\]\[sizeguard\]\[INVALID_OBJECT_SIZE\]' "$probe_tmp/$test_case.stderr" &&
        grep -aq 'regionStart=.*regionType=.*young=.*phase=.*bitCap=.*bitIdx=' "$probe_tmp/$test_case.stderr"; then
        passed=$((passed + 1))
        echo "GUARD_CASE result=PASS case=$test_case rc=$rc"
    else
        echo "GUARD_CASE result=FAIL case=$test_case rc=$rc"
        exit 1
    fi
done

echo "GUARD_FIRES_PROOF result=PASS cases=$passed/3"
