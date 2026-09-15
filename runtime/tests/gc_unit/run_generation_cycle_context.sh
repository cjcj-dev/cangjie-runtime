#!/usr/bin/env bash
# Managed cycle entry and explicit SATB ownership API tests.
# No product .cpp is compiled into either test artifact.
# GC_CYCLE_TESTABLE=1 selects the MRT_TESTABLE_INTERNALS product pair. The SATB
# companion (test_generation_satb_obligations.cpp) only exists in that
# configuration: it reaches product internals through testable-only friend
# access, so in the default configuration it is not built and SATB_RC=NOT_RUN.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit"
OUT="${GC_CYCLE_OUT:?set GC_CYCLE_OUT}"
LIB="${GCV2_RUNTIME_LIB_DIR:?set GCV2_RUNTIME_LIB_DIR to the product pair}"
mkdir -p "$OUT"
SDK="${CANGJIE_HOME:?set CANGJIE_HOME to matching compiler}"
HEADERS=$(python3 "$ROOT/runtime/build/resolve_runtime_headers.py" "$ROOT/runtime" "$LIB")
TEST_FLAGS=()
SATB_ENABLED=0
if [[ "${GC_CYCLE_TESTABLE:-0}" == 1 ]]; then
  TEST_FLAGS+=(-DMRT_TESTABLE_INTERNALS=1)
  SATB_ENABLED=1
fi
if [[ "${GC_CYCLE_REUSE_ELFS:-0}" != 1 ]]; then
"${CXX:-clang++}" -shared -fPIC -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti \
  -fvisibility-inlines-hidden "${TEST_FLAGS[@]}" \
  -I"$SRC" -I"$ROOT/runtime/src" -I"$ROOT/runtime/src/Heap" \
  -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include" \
  -I"$ROOT/runtime/include" -I"$HEADERS/include" \
  -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include" \
  "$SRC/test_generation_cycle_context.cpp" \
  -L"$LIB" -Wl,-rpath,"$LIB" -Wl,--exclude-libs,ALL \
  -lcangjie-runtime -lboundscheck -ldl -o "$OUT/libcycle_observer.so"
if [[ "$SATB_ENABLED" == 1 ]]; then
# Same entry point as cj_gc_unit (gc_unit_main.cpp): it owns the
# --gtest_filter=/--gtest_list_tests parsing that GC_OTHER_VM_TEST children
# are re-exec'd with (gc_unittest.hpp RunInOtherVm).
"${CXX:-clang++}" -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti \
  -fvisibility-inlines-hidden "${TEST_FLAGS[@]}" \
  -I"$SRC" -I"$ROOT/runtime/src" -I"$ROOT/runtime/src/Heap" \
  -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include" \
  -I"$ROOT/runtime/include" -I"$HEADERS/include" \
  -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include" \
  "$SRC/gc_unit_main.cpp" "$SRC/gc_cycle_sequence_fixture.cpp" \
  "$SRC/test_generation_satb_obligations.cpp" \
  -L"$LIB" -Wl,-rpath,"$LIB" -Wl,--exclude-libs,ALL \
  -lcangjie-runtime -lboundscheck -ldl -o "$OUT/generation_satb_obligations"
fi
cat > "$OUT/cycle.cj" <<'CJ'
foreign {
    func cycleExercise(): Int32
}
main(): Int64 {
    let result = spawn { => unsafe { cycleExercise() } }
    return Int64(result.get())
}
CJ
SDK="${CANGJIE_HOME:?set CANGJIE_HOME to matching compiler}"
LD_LIBRARY_PATH="${GC_UNIT_CJC_RUNTIME_LIB_DIR:?set GC_UNIT_CJC_RUNTIME_LIB_DIR to compiler host runtime}:$SDK/tools/lib:$SDK/third_party/llvm/lib" \
  "$SDK/bin/cjc" "$OUT/cycle.cj" -O0 --static-std -L "$LIB" -L "$OUT" -lcycle_observer -o "$OUT/generation_cycle_context"
fi
if [[ "$SATB_ENABLED" == 1 ]]; then
  sha256sum "$OUT/generation_satb_obligations"
fi
sha256sum "$OUT/generation_cycle_context" "$OUT/libcycle_observer.so" "$LIB/libcangjie-runtime.so" "$LIB/libboundscheck.so"
set +e
LD_LIBRARY_PATH="$OUT:$LIB:$SDK/runtime/lib/linux_x86_64_cjnative${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  cjGCInterval=3600s timeout 60s "$OUT/generation_cycle_context"
cycle_rc=$?
satb_rc=NOT_RUN
if [[ "$SATB_ENABLED" == 1 ]]; then
  LD_LIBRARY_PATH="$LIB:$SDK/runtime/lib/linux_x86_64_cjnative${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    timeout 60s "$OUT/generation_satb_obligations"
  satb_rc=$?
fi
printf "CYCLE_RC=%s SATB_RC=%s\n" "$cycle_rc" "$satb_rc"
[[ "$cycle_rc" == 0 && ( "$satb_rc" == 0 || "$satb_rc" == NOT_RUN ) ]]
