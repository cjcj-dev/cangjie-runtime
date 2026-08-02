#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(git -C "$script_dir" rev-parse --show-toplevel)
cpuset=${GCV2_CPUSET:-112-127}
runtime_lib_dir=${GCV2_RUNTIME_LIB_DIR:-$repo/runtime/output/temp/lib/x86_64_Release}
probe_tmp=$(mktemp -d /tmp/gcv2-nvi-probe.XXXXXX)

cleanup()
{
    unlink "$probe_tmp/gcv2-nvi-probe" 2>/dev/null || true
    rmdir "$probe_tmp"
}
trap cleanup EXIT

taskset -c "$cpuset" clang++ -std=gnu++14 -O2 -pthread \
    -I"$repo/runtime/src" "$repo/runtime/tests/generational_barrier_nvi_harness.cpp" \
    -L"$runtime_lib_dir" -Wl,-rpath,"$runtime_lib_dir" -lcangjie-runtime -lboundscheck \
    -o "$probe_tmp/gcv2-nvi-probe"
LD_LIBRARY_PATH="$runtime_lib_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    taskset -c "$cpuset" "$probe_tmp/gcv2-nvi-probe"
