#!/usr/bin/env bash
# Positive control for G-C1/G-C2/G-C3 VArray remset recording.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(git -C "$script_dir" rev-parse --show-toplevel)
cpuset=${GCV2_CPUSET:-112-127}
runtime_lib_dir=${GCV2_RUNTIME_LIB_DIR:-$repo/runtime/output/temp/lib/x86_64_Release}
probe_tmp=$(mktemp -d /tmp/gcv2-varraywb-probe.XXXXXX)

cleanup()
{
    unlink "$probe_tmp/varraywb-probe" 2>/dev/null || true
    rmdir "$probe_tmp" 2>/dev/null || true
}
trap cleanup EXIT

taskset -c "$cpuset" clang++ -std=gnu++14 -O2 -pthread -fno-rtti -fno-exceptions \
    -I"$repo/runtime/src" -I"$repo/runtime/src/Heap" -I"$repo/runtime/include" \
    -I"$repo/runtime/output/temp/include" \
    -I"$repo/runtime/third_party/third_party_bounds_checking_function/include" \
    "$repo/runtime/tests/varray_write_barrier_harness.cpp" \
    -L"$runtime_lib_dir" -Wl,-rpath,"$runtime_lib_dir" -lcangjie-runtime -lboundscheck \
    -o "$probe_tmp/varraywb-probe"
LD_LIBRARY_PATH="$runtime_lib_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    taskset -c "$cpuset" "$probe_tmp/varraywb-probe"
