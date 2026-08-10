#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# Structural probe for movable-stack watermark (#7). Links against built runtime SO.

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(git -C "$script_dir" rev-parse --show-toplevel)
cpuset=${GCV2_CPUSET:-}
# fail-closed: a hard-coded default used to taskset onto cores another lane had
# claimed (0-63 / 96-127 / 112-127 are all inside the reservable range).
[[ -n "$cpuset" ]] || { echo "set GCV2_CPUSET to cores you have claimed" >&2; exit 2; }
probe_tmp=$(mktemp -d /tmp/gcv2-stackgrow-probe.XXXXXX)

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
    local bc_inc=""
    if [[ -d "$repo/runtime/third_party/third_party_bounds_checking_function/include" ]]; then
        bc_inc="-I$repo/runtime/third_party/third_party_bounds_checking_function/include"
    fi
    # Prefer system / known boundscheck locations used by prior stack probes.
    local bc_lib=""
    for cand in \
        "$libdir" \
        "$repo/runtime/output/temp/lib/x86_64_Release" \
        "$repo/runtime/third_party/third_party_bounds_checking_function/lib" \
        /usr/lib /usr/local/lib; do
        if [[ -f "$cand/libboundscheck.so" || -f "$cand/libboundscheck.a" ]]; then
            bc_lib="-L$cand -lboundscheck"
            break
        fi
    done
    taskset -c "$cpuset" clang++ -std=gnu++14 -O2 -pthread -fno-rtti -fno-exceptions \
        -I"$src_root" -I"$src_root/Heap" -I"$repo/runtime/include" \
        -I"$repo/runtime/output/temp/include" \
        $bc_inc \
        "$repo/runtime/tests/stack_grow_harness.cpp" \
        -L"$libdir" -Wl,-rpath,"$libdir" -lcangjie-runtime $bc_lib \
        -o "$probe_tmp/stack-grow" 2>"$probe_tmp/compile.err"
}

if [[ -n "$runtime_lib_dir" && -f "$runtime_lib_dir/libcangjie-runtime.so" ]]; then
    compile_with_so "$runtime_lib_dir"
elif [[ -f "$repo/runtime/output/temp/lib/x86_64_Release/libcangjie-runtime.so" ]]; then
    compile_with_so "$repo/runtime/output/temp/lib/x86_64_Release"
elif [[ -f "$repo/runtime/output/temp/lib/x86_64_Relwithdebinfo/libcangjie-runtime.so" ]]; then
    compile_with_so "$repo/runtime/output/temp/lib/x86_64_Relwithdebinfo"
else
    echo "STACKGROW_PROBE no runtime SO; set GCV2_RUNTIME_LIB_DIR" >&2
    cat "$probe_tmp/compile.err" 2>/dev/null || true
    exit 2
fi

so_dir=$(ldd "$probe_tmp/stack-grow" | awk '/libcangjie-runtime/{print $3}' | xargs -r dirname)
export LD_LIBRARY_PATH="${so_dir:-$runtime_lib_dir}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

run_ok()
{
    local case=$1
    set +e
    MRT_GCV2_STACK_WATERMARK_VERIFY=1 \
        taskset -c "$cpuset" "$probe_tmp/stack-grow" "$case" \
        >"$probe_tmp/$case.stdout" 2>"$probe_tmp/$case.stderr"
    local rc=$?
    set -e
    if [[ "$rc" -eq 0 ]] && grep -aq "HARNESS_OK case=$case" "$probe_tmp/$case.stderr"; then
        echo "STACKGROW_CASE result=PASS case=$case rc=$rc"
        return 0
    fi
    echo "STACKGROW_CASE result=FAIL case=$case rc=$rc"
    cat "$probe_tmp/$case.stderr"
    return 1
}

passed=0
run_ok logical_stable && passed=$((passed + 1))
run_ok absolute_desync && passed=$((passed + 1))
run_ok resume_token && passed=$((passed + 1))
run_ok wrong_offset && passed=$((passed + 1))
run_ok no_stw && passed=$((passed + 1))
run_ok gen_monotonic && passed=$((passed + 1))

echo "STACKGROW_PROBE result=PASS cases=$passed/6"
[[ "$passed" -eq 6 ]]
