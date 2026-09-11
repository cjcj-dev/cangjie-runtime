#!/usr/bin/env bash
# Managed cycle entry and explicit SATB ownership API tests.
# No product .cpp is compiled into either test artifact.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit"
OUT="${GC_CYCLE_OUT:?set GC_CYCLE_OUT}"
LIB="${GCV2_RUNTIME_LIB_DIR:?set GCV2_RUNTIME_LIB_DIR to the product pair}"
mkdir -p "$OUT"
SDK="${CANGJIE_HOME:?set CANGJIE_HOME to matching compiler}"
HEADERS=$(python3 "$ROOT/runtime/build/resolve_runtime_headers.py" "$ROOT/runtime" "$LIB")
if [[ "${GC_CYCLE_REUSE_ELFS:-0}" != 1 ]]; then
"${CXX:-clang++}" -shared -fPIC -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti \
  -fvisibility-inlines-hidden \
  -I"$SRC" -I"$ROOT/runtime/src" -I"$ROOT/runtime/src/Heap" \
  -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include" \
  -I"$ROOT/runtime/include" -I"$HEADERS/include" \
  -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include" \
  "$SRC/test_generation_cycle_context.cpp" \
  -L"$LIB" -Wl,-rpath,"$LIB" -Wl,--exclude-libs,ALL \
  -lcangjie-runtime -lboundscheck -ldl -o "$OUT/libcycle_observer.so"
"${CXX:-clang++}" -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti \
  -fvisibility-inlines-hidden \
  -I"$SRC" -I"$ROOT/runtime/src" -I"$ROOT/runtime/src/Heap" \
  -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include" \
  -I"$ROOT/runtime/include" -I"$HEADERS/include" \
  -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include" \
  "$SRC/test_generation_satb_obligations.cpp" \
  -L"$LIB" -Wl,-rpath,"$LIB" -Wl,--exclude-libs,ALL \
  -lcangjie-runtime -lboundscheck -ldl -o "$OUT/generation_satb_obligations"
cat > "$OUT/cycle.cj" <<'CJ'
foreign {
    func cycleExercise(): Int32
}
main(): Int64 {
    return Int64(unsafe { cycleExercise() })
}
CJ
SDK="${CANGJIE_HOME:?set CANGJIE_HOME to matching compiler}"
LD_LIBRARY_PATH="$LIB:$SDK/runtime/lib/linux_x86_64_cjnative:$SDK/tools/lib:$SDK/third_party/llvm/lib" \
  "$SDK/bin/cjc" "$OUT/cycle.cj" -O0 --static-std -L "$OUT" -lcycle_observer -o "$OUT/generation_cycle_context"
fi
sha256sum "$OUT/generation_satb_obligations" "$OUT/generation_cycle_context" "$OUT/libcycle_observer.so" "$LIB/libcangjie-runtime.so" "$LIB/libboundscheck.so"
set +e
LD_LIBRARY_PATH="$OUT:$LIB:$SDK/runtime/lib/linux_x86_64_cjnative${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  cjGCInterval=3600s timeout 60s "$OUT/generation_cycle_context"
cycle_rc=$?
LD_LIBRARY_PATH="$LIB:$SDK/runtime/lib/linux_x86_64_cjnative${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  timeout 60s "$OUT/generation_satb_obligations"
satb_rc=$?
printf "CYCLE_RC=%s SATB_RC=%s\n" "$cycle_rc" "$satb_rc"
[[ "$cycle_rc" == 0 && "$satb_rc" == 0 ]]
