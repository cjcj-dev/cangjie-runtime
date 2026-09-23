#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.

# Positive control for the ported ZBarrier::self_heal loop. Needs a built runtime SO:
# the unit links the real HealSlot rather than a copy of them.
#
#   RTLIB=<dir with libcangjie-runtime.so> bash runtime/tests/run_zgc_self_heal_loop_probe.sh

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
runtime_dir=$(cd "${script_dir}/.." && pwd)
rtlib=${RTLIB:-${GCV2_RUNTIME_LIB_DIR:-}}

if [[ -z "${rtlib}" || ! -f "${rtlib}/libcangjie-runtime.so" ]]; then
    echo "ZGC_SELF_HEAL_LOOP_UNIT SKIP no libcangjie-runtime.so under ${rtlib}" >&2
    exit 2
fi
runtime_output_root="${GCV2_RUNTIME_OUTPUT_ROOT:-$(realpath -m "${rtlib}/../..")}"

build_dir=$(mktemp -d "${TMPDIR:-/tmp}/zgc-self-heal-loop.XXXXXX")
trap 'rm -rf "${build_dir}"' EXIT

compiler=${CXX:-c++}
# Same language level and aliasing rule the runtime itself is built with; HeapSlot is a
# union over the slot word and the test aliases that word directly.
"${compiler}" -std=gnu++14 -fno-strict-aliasing -fno-exceptions -fno-rtti -Wall -Wextra \
    -DMRT_USE_CJTHREAD_RENAME -DMRT_USE_COPYGC -DDISABLE_VERSION_CHECK \
    -I"${runtime_dir}/src" -I"${runtime_dir}/include" -I"${runtime_output_root}/include" \
    -I"${runtime_dir}/third_party/third_party_bounds_checking_function/include" \
    "${script_dir}/zgc_self_heal_loop_unit.cpp" \
    -L"${rtlib}" -lcangjie-runtime -Wl,-rpath,"${rtlib}" \
    -o "${build_dir}/zgc_self_heal_loop_unit"

# The four product self-heal cases retain their slot-value assertions.
LD_LIBRARY_PATH="${rtlib}:${LD_LIBRARY_PATH:-}" "${build_dir}/zgc_self_heal_loop_unit"
