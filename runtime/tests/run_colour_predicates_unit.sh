#!/usr/bin/env bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
runtime_dir=$(cd "${script_dir}/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/colour-predicates.XXXXXX")
trap 'rm -rf "${build_dir}"' EXIT

compiler=${CXX:-c++}
"${compiler}" -std=c++14 -Wall -Wextra -Werror -I"${runtime_dir}/src" \
    "${script_dir}/colour_predicates_unit.cpp" -o "${build_dir}/colour_predicates_unit"
"${build_dir}/colour_predicates_unit"
