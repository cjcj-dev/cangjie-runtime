#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
: "${GCV2_RUNTIME_LIB_DIR:?}" "${SIGNAL_TEST_OUTPUT:?}"
output_root=${GCV2_RUNTIME_OUTPUT_ROOT:-$(realpath -m "$GCV2_RUNTIME_LIB_DIR/../..")}
mkdir -p "$SIGNAL_TEST_OUTPUT"
clang++ -std=c++17 -O0 -g -pthread -fno-rtti \
    -I"$repo/runtime/src" -I"$repo/runtime/src/Heap" -I"$repo/runtime/include" \
    -I"$repo/runtime/src/Heap/z/os/linux" \
    -I"$output_root/include" \
    -I"$repo/runtime/third_party/third_party_bounds_checking_function/include" \
    "$repo/runtime/tests/signal_fatal_watermark.cpp" \
    -L"$GCV2_RUNTIME_LIB_DIR" -Wl,-rpath,"$GCV2_RUNTIME_LIB_DIR" \
    -lcangjie-runtime -lboundscheck -ldl -o "$SIGNAL_TEST_OUTPUT/signal-fatal-watermark"
sha256sum "$SIGNAL_TEST_OUTPUT/signal-fatal-watermark" "$GCV2_RUNTIME_LIB_DIR/"*.so > "$SIGNAL_TEST_OUTPUT/identity.sha256"
