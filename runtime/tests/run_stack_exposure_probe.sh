#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# Structural probe for StackExposureHook (#5). Links against built runtime SO.

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(git -C "$script_dir" rev-parse --show-toplevel)
cpuset=${GCV2_CPUSET:-0-63}
probe_tmp=$(mktemp -d /tmp/gcv2-exposehook-probe.XXXXXX)

cleanup()
{
    rm -rf "$probe_tmp"
}
trap cleanup EXIT

runtime_lib_dir=${GCV2_RUNTIME_LIB_DIR:-}
src_root="$repo/runtime/src"

compile_with_so()
{
    local libdir=$1
    taskset -c "$cpuset" clang++ -std=gnu++14 -O2 -pthread -fno-rtti -fno-exceptions \
        -I"$src_root" -I"$src_root/Heap" -I"$repo/runtime/include" \
        -I"$repo/runtime/output/temp/include" \
        -I"$repo/runtime/third_party/third_party_bounds_checking_function/include" \
        "$repo/runtime/tests/stack_exposure_harness.cpp" \
        -L"$libdir" -Wl,-rpath,"$libdir" -lcangjie-runtime -lboundscheck \
        -o "$probe_tmp/stack-exposure" 2>"$probe_tmp/compile.err"
}

if [[ -n "$runtime_lib_dir" && -f "$runtime_lib_dir/libcangjie-runtime.so" ]]; then
    compile_with_so "$runtime_lib_dir"
elif [[ -f "$repo/runtime/output/temp/lib/x86_64_Release/libcangjie-runtime.so" ]]; then
    compile_with_so "$repo/runtime/output/temp/lib/x86_64_Release"
elif [[ -f "$repo/runtime/output/temp/lib/x86_64_Relwithdebinfo/libcangjie-runtime.so" ]]; then
    compile_with_so "$repo/runtime/output/temp/lib/x86_64_Relwithdebinfo"
else
    echo "EXPOSEHOOK_PROBE no runtime SO; set GCV2_RUNTIME_LIB_DIR" >&2
    cat "$probe_tmp/compile.err" 2>/dev/null || true
    exit 2
fi

so_dir=$(ldd "$probe_tmp/stack-exposure" | awk '/libcangjie-runtime/{print $3}' | xargs -r dirname)
export LD_LIBRARY_PATH="${so_dir:-$runtime_lib_dir}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

run_ok()
{
    local case=$1
    set +e
    MRT_GCV2_STACK_EXPOSURE_VERIFY=1 \
        taskset -c "$cpuset" "$probe_tmp/stack-exposure" "$case" \
        >"$probe_tmp/$case.stdout" 2>"$probe_tmp/$case.stderr"
    local rc=$?
    set -e
    if [[ "$rc" -eq 0 ]] && grep -aq "HARNESS_OK case=$case" "$probe_tmp/$case.stderr"; then
        echo "EXPOSEHOOK_CASE result=PASS case=$case rc=$rc"
        return 0
    fi
    echo "EXPOSEHOOK_CASE result=FAIL case=$case rc=$rc"
    cat "$probe_tmp/$case.stderr"
    return 1
}

passed=0
run_ok fire_on_cross && passed=$((passed + 1))
run_ok observe_cross && passed=$((passed + 1))
run_ok empty_process && passed=$((passed + 1))
run_ok no_stw && passed=$((passed + 1))
run_ok already_scanned && passed=$((passed + 1))
run_ok done_epoch && passed=$((passed + 1))

echo "EXPOSEHOOK_PROBE result=PASS cases=$passed/6"
[[ "$passed" -eq 6 ]]
