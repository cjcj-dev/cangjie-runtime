#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# Structural probe for StackWatermark (#1). Links StackWatermark.cpp only
# (no full runtime) so it can run without InitCJRuntime.

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(git -C "$script_dir" rev-parse --show-toplevel)
cpuset=${GCV2_CPUSET:-}
# fail-closed: a hard-coded default used to taskset onto cores another lane had
# claimed (0-63 / 96-127 / 112-127 are all inside the reservable range).
[[ -n "$cpuset" ]] || { echo "set GCV2_CPUSET to cores you have claimed" >&2; exit 2; }
probe_tmp=$(mktemp -d /tmp/gcv2-stackmark-probe.XXXXXX)

cleanup()
{
    rm -rf "$probe_tmp"
}
trap cleanup EXIT

# Minimal compile: StackWatermark.cpp + harness. CHECK_DETAIL needs Base/Log.
# Prefer linking against built runtime SO when available.
runtime_lib_dir=${GCV2_RUNTIME_LIB_DIR:-}
src_root="$repo/runtime/src"

compile_with_so()
{
    local libdir=$1
    taskset -c "$cpuset" clang++ -std=gnu++14 -O2 -pthread -fno-rtti -fno-exceptions \
        -I"$src_root" -I"$src_root/Heap" -I"$repo/runtime/include" \
        -I"$repo/runtime/output/temp/include" \
        -I"$repo/runtime/third_party/third_party_bounds_checking_function/include" \
        "$repo/runtime/tests/stack_watermark_harness.cpp" \
        -L"$libdir" -Wl,-rpath,"$libdir" -lcangjie-runtime -lboundscheck \
        -o "$probe_tmp/stack-watermark" 2>"$probe_tmp/compile.err"
}

if [[ -n "$runtime_lib_dir" && -f "$runtime_lib_dir/libcangjie-runtime.so" ]]; then
    compile_with_so "$runtime_lib_dir"
elif [[ -f "$repo/runtime/output/temp/lib/x86_64_Release/libcangjie-runtime.so" ]]; then
    compile_with_so "$repo/runtime/output/temp/lib/x86_64_Release"
elif [[ -f "$repo/runtime/output/temp/lib/x86_64_Relwithdebinfo/libcangjie-runtime.so" ]]; then
    compile_with_so "$repo/runtime/output/temp/lib/x86_64_Relwithdebinfo"
else
    echo "STACKMARK_PROBE no runtime SO; set GCV2_RUNTIME_LIB_DIR" >&2
    cat "$probe_tmp/compile.err" 2>/dev/null || true
    exit 2
fi

libdir=$(dirname "$(readlink -f "$probe_tmp/stack-watermark" 2>/dev/null || echo "$probe_tmp")")
# Resolve rpath from ldd
so_dir=$(ldd "$probe_tmp/stack-watermark" | awk '/libcangjie-runtime/{print $3}' | xargs -r dirname)
export LD_LIBRARY_PATH="${so_dir:-$runtime_lib_dir}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

run_ok()
{
    local case=$1
    set +e
    MRT_GCV2_STACK_WATERMARK_VERIFY=1 \
        taskset -c "$cpuset" "$probe_tmp/stack-watermark" "$case" \
        >"$probe_tmp/$case.stdout" 2>"$probe_tmp/$case.stderr"
    local rc=$?
    set -e
    if [[ "$rc" -eq 0 ]] && grep -aq "HARNESS_OK case=$case" "$probe_tmp/$case.stderr"; then
        echo "STACKMARK_CASE result=PASS case=$case rc=$rc"
        return 0
    fi
    echo "STACKMARK_CASE result=FAIL case=$case rc=$rc"
    cat "$probe_tmp/$case.stderr"
    return 1
}

run_abort()
{
    local case=$1
    local needle=$2
    set +e
    MRT_GCV2_STACK_WATERMARK_VERIFY=1 \
        taskset -c "$cpuset" "$probe_tmp/stack-watermark" "$case" \
        >"$probe_tmp/$case.stdout" 2>"$probe_tmp/$case.stderr"
    local rc=$?
    set -e
    # CHECK_DETAIL aborts → typically 134 (SIGABRT) or 6.
    if [[ "$rc" -ne 0 ]] && grep -aqE "$needle" "$probe_tmp/$case.stderr"; then
        echo "STACKMARK_CASE result=PASS case=$case rc=$rc inject_fired=1"
        return 0
    fi
    echo "STACKMARK_CASE result=FAIL case=$case rc=$rc (expected abort + needle)"
    cat "$probe_tmp/$case.stderr"
    return 1
}

run_verify_off()
{
    local case=$1
    set +e
    env -u MRT_GCV2_STACK_WATERMARK_VERIFY \
        taskset -c "$cpuset" "$probe_tmp/stack-watermark" "$case" \
        >"$probe_tmp/$case.stdout" 2>"$probe_tmp/$case.stderr"
    local rc=$?
    set -e
    if [[ "$rc" -eq 0 ]] && grep -aq "HARNESS_OK case=$case" "$probe_tmp/$case.stderr"; then
        echo "STACKMARK_CASE result=PASS case=$case rc=$rc verify=off"
        return 0
    fi
    echo "STACKMARK_CASE result=FAIL case=$case rc=$rc verify=off"
    cat "$probe_tmp/$case.stderr"
    return 1
}

passed=0
run_ok happy && passed=$((passed + 1))
run_ok park_ok && passed=$((passed + 1))
run_ok create_closed && passed=$((passed + 1))
run_abort illegal_transition 'ILLEGAL_TRANSITION|begin while SCANNING' && passed=$((passed + 1))
run_abort dual_owner 'OWNER_NOT_UNIQUE|ILLEGAL_TRANSITION' && passed=$((passed + 1))
run_abort exit_scanning 'EXIT_WHILE_SCANNING' && passed=$((passed + 1))
run_verify_off coverage_guard && passed=$((passed + 1))

echo "STACKMARK_PROBE result=PASS cases=$passed/7"
[[ "$passed" -eq 7 ]]
