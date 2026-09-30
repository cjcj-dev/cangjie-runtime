#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
lib=$(realpath "$1")
output=$2
mkdir -p "$output"
clang++ -std=c++17 -O0 -g -pthread -fno-rtti \
    -I"$repo/runtime/src" -I"$repo/runtime/src/Heap" -I"$repo/runtime/include" \
    -I"$repo/runtime/src/Heap/z/os/linux" -I"$lib/../../include" \
    -I"$repo/runtime/third_party/third_party_bounds_checking_function/include" \
    "$repo/runtime/tests/signal_shutdown.cpp" \
    -L"$lib" -lcangjie-runtime -lboundscheck -ldl -o "$output/signal-shutdown"
sha256sum "$output/signal-shutdown" "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so"
