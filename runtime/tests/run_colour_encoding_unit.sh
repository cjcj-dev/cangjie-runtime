#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "$0")" && pwd)"
runtime_dir="$(cd "${script_dir}/.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/colour-encoding-unit.XXXXXX")"
trap 'rm -rf "${build_dir}"' EXIT

"${CXX:-c++}" -std=c++14 -Wall -Wextra -Werror \
    -I"${runtime_dir}/src" \
    "${script_dir}/colour_encoding_unit.cpp" -o "${build_dir}/colour_encoding_unit"
"${build_dir}/colour_encoding_unit"
