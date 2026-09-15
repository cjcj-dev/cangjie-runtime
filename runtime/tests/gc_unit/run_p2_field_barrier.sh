#!/usr/bin/env bash
# Real managed mark-start entry. Neither product source nor a model is linked
# into the test observer. Control arms reuse these exact test artifacts.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit"
OUT="${P2_FIELD_OUT:?set P2_FIELD_OUT}"
LIB="${GCV2_RUNTIME_LIB_DIR:?set product runtime/bounds pair}"
SDK="${CANGJIE_HOME:?set matching compiler SDK}"
HOST="${GC_UNIT_CJC_RUNTIME_LIB_DIR:?set compiler host runtime}"
HEADERS=$(python3 "$ROOT/runtime/build/resolve_runtime_headers.py" "$ROOT/runtime" "$LIB")
mkdir -p "$OUT"
"${CXX:-clang++}" -shared -fPIC -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti \
  -fvisibility-inlines-hidden -DMRT_TESTABLE_INTERNALS=1 -DMRT_GC_UNIT_TESTS=1 \
  -I"$SRC" -I"$ROOT/runtime/src" -I"$ROOT/runtime/src/Heap" \
  -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include" \
  -I"$ROOT/runtime/include" -I"$HEADERS/include" \
  -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include" \
  "$SRC/test_p2_field_barrier.cpp" -L"$LIB" -Wl,-rpath,"$LIB" -Wl,--exclude-libs,ALL \
  -lcangjie-runtime -lboundscheck -ldl -o "$OUT/libp2_field_barrier.so"
if [[ "${P2_PLAIN_MAIN:-0}" == 1 ]]; then
cat > "$OUT/p2_field_barrier.cj" <<'CJ'
foreign { func p2FieldBarrierExercise(): Int32 }
main(): Int64 {
    return Int64(unsafe { p2FieldBarrierExercise() })
}
CJ
else
# Keep the original managed/spawn coverage for the compiler literal-producer
# handoff. The plain-main arm only isolates the independent field mechanisms.
cat > "$OUT/p2_field_barrier.cj" <<'CJ'
foreign { func p2FieldBarrierExercise(): Int32 }
main(): Int64 {
    let result = spawn { => unsafe { p2FieldBarrierExercise() } }
    return Int64(result.get())
}
CJ
fi
ENTRY="${P2_TEST_ENTRY:-p2FieldBarrierExercise}"
[[ "$ENTRY" =~ ^[A-Za-z][A-Za-z0-9_]*$ ]] || exit 2
sed -i "s/p2FieldBarrierExercise/$ENTRY/g" "$OUT/p2_field_barrier.cj"
LD_LIBRARY_PATH="$HOST:$SDK/tools/lib:$SDK/third_party/llvm/lib" \
  "${P2_CJC:-$SDK/bin/cjc}" "$OUT/p2_field_barrier.cj" -O0 --static-std -L "$LIB" -L "$OUT" \
  -lp2_field_barrier -o "$OUT/p2_field_barrier"
sha256sum "$OUT/p2_field_barrier" "$OUT/libp2_field_barrier.so" "$LIB/libcangjie-runtime.so" "$LIB/libboundscheck.so"
LD_LIBRARY_PATH="$OUT:$LIB:$SDK/runtime/lib/linux_x86_64_cjnative${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  cjGCInterval=3600s timeout 60s "$OUT/p2_field_barrier"
