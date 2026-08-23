#!/bin/bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.
#
# Manual arm of the colour state machine table.
#
# The judgement lives in the configure arm: runtime/CMakeLists.txt try_compiles
# runtime/tests/colour_state_machine_probe.cpp and every claim is a static_assert, so this
# script cannot pass while the build is red or vice versa. What it adds is the readable form --
# the actual colour words of the collision witness, and the collision counts per dropped family
# -- for a human deciding whether the fifth family is worth wiring.
#
# Needs: a host g++/clang++ with C++14. No built runtime, no cpuset, no fault injection.
#
#   bash runtime/tests/run_colour_table_witness.sh

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="${here}/colour_state_machine_probe.cpp"
inc="${here}/../src"
cxx="${CXX:-g++}"
out="$(mktemp -d)/colour_table_witness"

extra=()
if "${cxx}" --version 2>/dev/null | grep -qi clang; then
    # clang's default constexpr step budget is a million; the table needs more. gcc's default
    # ops-limit is large enough.
    extra+=(-fconstexpr-steps=100000000)
fi

echo "== building ${src} with ${cxx} =="
"${cxx}" -std=c++14 -I "${inc}" -DMRT_C4TABLE_PRINT_WITNESS "${extra[@]}" "${src}" -o "${out}"

echo "== witness =="
"${out}"

cat <<'NOTE'

Reading this:
  None / Ghost must be 0.  A non-zero None means the action function consults state that no
  colour encoding carries; a non-zero Ghost means the collision check answers yes regardless of
  its input, so every other number on the line is worthless.
  Every other family must be > 0.  That number is how many (epoch, event, colour-pair) triples
  that configuration cannot decide.
  Finalizable > 0 is the family we do not have.  The witness pair above is the cheapest cell:
  same colour word after the live projection, different required action -- fast path for a
  strongly marked target, upgrade-to-strong for a resurrection-marked one.
NOTE
