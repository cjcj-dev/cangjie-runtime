#!/usr/bin/env bash
# Address-reuse and measured-RSS acceptance for registeredTypeInfos.
# Usage: TYPEINFO_RUNTIME_LIB_DIR=/positive/lib TYPEINFO_NEGATIVE_RUNTIME_LIB_DIR=/negative/lib \
#   bash runtime/tests/run_typeinfo_reuse_probe.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/runtime/tests"
OUT="${TYPEINFO_REUSE_OUT:-$SRC/build_typeinfo_reuse}"
RUNTIME_LIB_DIR="${TYPEINFO_RUNTIME_LIB_DIR:-}"
NEGATIVE_RUNTIME_LIB_DIR="${TYPEINFO_NEGATIVE_RUNTIME_LIB_DIR:-}"
CXX="${CXX:-clang++}"
ATTEMPTS="${TYPEINFO_REUSE_ATTEMPTS:-10000}"
RSS_ENTRIES="${TYPEINFO_RSS_ENTRIES:-1000000}"

if [[ -z "$RUNTIME_LIB_DIR" || ! -f "$RUNTIME_LIB_DIR/libcangjie-runtime.so" ]]; then
    echo "error: set TYPEINFO_RUNTIME_LIB_DIR to a dir containing libcangjie-runtime.so" >&2
    exit 2
fi
if [[ -z "$NEGATIVE_RUNTIME_LIB_DIR" || ! -f "$NEGATIVE_RUNTIME_LIB_DIR/libcangjie-runtime.so" ]]; then
    echo "error: set TYPEINFO_NEGATIVE_RUNTIME_LIB_DIR to the fault-injected runtime dir" >&2
    exit 2
fi
mkdir -p "$OUT"

INC_FLAGS=(
    -I"$ROOT/runtime/src"
    -I"$ROOT/runtime/src/Heap"
    -I"$ROOT/runtime/include"
    -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include"
)
if [[ -d "$ROOT/runtime/output/temp/include" ]]; then
    INC_FLAGS+=(-I"$ROOT/runtime/output/temp/include")
fi
COMMON_FLAGS=(-std=gnu++17 -O2 -g -Wall -Wextra -pthread -fno-rtti "${INC_FLAGS[@]}")
POSITIVE_LINK_FLAGS=(-L"$RUNTIME_LIB_DIR" -Wl,-rpath,"$RUNTIME_LIB_DIR" -lcangjie-runtime -lboundscheck -ldl)
NEGATIVE_LINK_FLAGS=(-L"$NEGATIVE_RUNTIME_LIB_DIR" -Wl,-rpath,"$NEGATIVE_RUNTIME_LIB_DIR" -lcangjie-runtime -lboundscheck -ldl)

"$CXX" -std=gnu++17 -O2 -fPIC -shared "$SRC/typeinfo_reuse_plugin.cpp" -o "$OUT/libtypeinfo_a.so"
"$CXX" -std=gnu++17 -O2 -fPIC -shared -DABASTRESS_PLUGIN_B=1 \
    "$SRC/typeinfo_reuse_plugin.cpp" -o "$OUT/libtypeinfo_b.so"
"$CXX" "${COMMON_FLAGS[@]}" "$SRC/typeinfo_reuse_harness.cpp" \
    "${POSITIVE_LINK_FLAGS[@]}" -o "$OUT/typeinfo_reuse_positive"
"$CXX" "${COMMON_FLAGS[@]}" -DABASTRESS_NEGATIVE_RUNTIME=1 "$SRC/typeinfo_reuse_harness.cpp" \
    "${NEGATIVE_LINK_FLAGS[@]}" -o "$OUT/typeinfo_reuse_negative"

echo "LINKED_RUNTIME=$RUNTIME_LIB_DIR"
echo "LINKED_NEGATIVE_RUNTIME=$NEGATIVE_RUNTIME_LIB_DIR"
LD_LIBRARY_PATH="$RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$OUT/typeinfo_reuse_positive" reuse "$OUT/libtypeinfo_a.so" "$OUT/libtypeinfo_b.so" "$ATTEMPTS"
LD_LIBRARY_PATH="$NEGATIVE_RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$OUT/typeinfo_reuse_negative" reuse "$OUT/libtypeinfo_a.so" "$OUT/libtypeinfo_b.so" "$ATTEMPTS"
LD_LIBRARY_PATH="$RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$OUT/typeinfo_reuse_positive" rss "$RSS_ENTRIES"
