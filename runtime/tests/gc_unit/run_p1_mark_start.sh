#!/usr/bin/env bash
# Real managed mark-start entry. Neither product source nor a model is linked
# into the test observer. Control arms reuse these exact test artifacts.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit"
OUT="${P1_MARK_START_OUT:?set P1_MARK_START_OUT}"
LIB="${GCV2_RUNTIME_LIB_DIR:?set product runtime/bounds pair}"
SDK="${CANGJIE_HOME:?set matching compiler SDK}"
HOST="${GC_UNIT_CJC_RUNTIME_LIB_DIR:?set compiler host runtime}"
HEADERS=$(python3 "$ROOT/runtime/build/resolve_runtime_headers.py" "$ROOT/runtime" "$LIB")
mkdir -p "$OUT"
"${CXX:-clang++}" -shared -fPIC -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti \
  -fvisibility-inlines-hidden -DMRT_TESTABLE_INTERNALS=1 \
  -I"$SRC" -I"$ROOT/runtime/src" -I"$ROOT/runtime/src/Heap" \
  -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include" \
  -I"$ROOT/runtime/include" -I"$HEADERS/include" \
  -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include" \
  "$SRC/test_p1_mark_start.cpp" -L"$LIB" -Wl,-rpath,"$LIB" -Wl,--exclude-libs,ALL \
  -lcangjie-runtime -lboundscheck -ldl -o "$OUT/libp1_mark_start.so"
cat > "$OUT/p1_mark_start.cj" <<'CJ'
foreign { func p1MarkStartExercise(): Int32 }
main(): Int64 {
    let result = spawn { => unsafe { p1MarkStartExercise() } }
    return Int64(result.get())
}
CJ
LD_LIBRARY_PATH="$HOST:$SDK/tools/lib:$SDK/third_party/llvm/lib" \
  "$SDK/bin/cjc" "$OUT/p1_mark_start.cj" -O0 --static-std -L "$LIB" -L "$OUT" \
  -lp1_mark_start -o "$OUT/p1_mark_start"
sha256sum "$OUT/p1_mark_start" "$OUT/libp1_mark_start.so" "$LIB/libcangjie-runtime.so" "$LIB/libboundscheck.so"
LD_LIBRARY_PATH="$OUT:$LIB:$SDK/runtime/lib/linux_x86_64_cjnative${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  cjGCInterval=3600s timeout 60s "$OUT/p1_mark_start"
