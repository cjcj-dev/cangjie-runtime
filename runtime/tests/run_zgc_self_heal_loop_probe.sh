#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.

# Positive control for the ported ZBarrier::self_heal loop. Needs a built runtime SO:
# the unit links the real HealSlot / diagnostics rather than a copy of them.
#
#   RTLIB=<dir with libcangjie-runtime.so> bash runtime/tests/run_zgc_self_heal_loop_probe.sh

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
runtime_dir=$(cd "${script_dir}/.." && pwd)
rtlib=${RTLIB:-${runtime_dir}/output/temp/lib/x86_64_Release}

if [[ ! -f "${rtlib}/libcangjie-runtime.so" ]]; then
    echo "ZGC_SELF_HEAL_LOOP_UNIT SKIP no libcangjie-runtime.so under ${rtlib}" >&2
    exit 2
fi

build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zgc-self-heal-loop.XXXXXX")
trap 'rm -rf "${build_dir}"' EXIT

compiler=${CXX:-c++}
# Same language level and aliasing rule the runtime itself is built with; HeapSlot is a
# union over the slot word and the test aliases that word directly.
"${compiler}" -std=gnu++14 -fno-strict-aliasing -fno-exceptions -fno-rtti -Wall -Wextra \
    -DMRT_USE_CJTHREAD_RENAME -DMRT_USE_COPYGC -DDISABLE_VERSION_CHECK \
    -I"${runtime_dir}/src" -I"${runtime_dir}/include" -I"${runtime_dir}/output/temp/include" \
    -I"${runtime_dir}/third_party/third_party_bounds_checking_function/include" \
    "${script_dir}/zgc_self_heal_loop_unit.cpp" \
    -L"${rtlib}" -lcangjie-runtime -Wl,-rpath,"${rtlib}" \
    -o "${build_dir}/zgc_self_heal_loop_unit"

rc=0

# The gate must be set before load: ZgcSelfHealDiag caches it in a static initialiser.
run_mode() {
    local mode="$1" why="$2" out="$3"; shift 3
    MRT_GCV2_ZGC_SELFHEAL=1 LD_LIBRARY_PATH="${rtlib}:${LD_LIBRARY_PATH:-}" \
        "${build_dir}/zgc_self_heal_loop_unit" ${mode} > "${out}" 2>&1 || true
    cat "${out}"
    grep -q "ZGC_SELF_HEAL_LOOP_UNIT PASS" "${out}" \
        || { echo "ZGC_SELF_HEAL_LOOP_UNIT FAIL assertions (${why})"; rc=1; }
    # The arm counts are the judgement: a run where retry / fastpath_exit / spin_alarm
    # stay 0 has not tested what this port adds, however green the assertions look.
    local census
    census=$(grep "zgcselfheal\]\[census\] why=${why}" "${out}" || true)
    if [[ -z "${census}" ]]; then
        echo "ZGC_SELF_HEAL_LOOP_UNIT FAIL no census line (${why})"
        rc=1
        return
    fi
    local expect
    for expect in "$@"; do
        grep -q " ${expect} " <<<"${census} " \
            || { echo "ZGC_SELF_HEAL_LOOP_UNIT FAIL census missing ${expect} (${why})"; rc=1; }
    done
}

# :91-96 / :98-101 / :103-107 / :73-79
run_mode "" unit "${build_dir}/out.txt" \
    "enter=3" "null_skip=1" "healed=2" "fastpath_exit=1" "retry=2" "iter_max=2" "spin_alarm=0"

# Livelock sentinel: it must fire, and the loop must still converge afterwards.
# Separate process so the four-figure retry count cannot bury the arm counts above.
#
# 1025, not 1024: the competing write happens when the fast path is asked about the
# prev value, i.e. after that iteration's CAS has already read the slot. So the last
# bump costs one more lost CAS before the retry with the settled value lands.
run_mode spin unit-spin "${build_dir}/out-spin.txt" \
    "enter=1" "healed=1" "retry=1025" "iter_max=1025" "spin_alarm=1"

exit "${rc}"
