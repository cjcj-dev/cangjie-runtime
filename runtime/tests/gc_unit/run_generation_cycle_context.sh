#!/usr/bin/env bash
# Managed cycle entry and explicit SATB ownership API tests.
# No product .cpp is compiled into either test artifact.
# The test translation units must see the same compile-time configuration as
# the product SO they bind (run_standalone.sh derives it the same way): the
# testable SO is built with MRT_TESTABLE_INTERNALS=1 and, through
# runtime/CMakeLists.txt "MRT_GC_UNIT_TESTS OR MRT_TESTABLE_INTERNALS",
# MRT_GC_UNIT_TESTS=1, and both macros gate data members (zDriver.hpp
# CollectorResources, zDriverPort.hpp GCDriverPort, WCollector.h). A TU built
# with a different set reads product objects at the wrong offsets.
# GC_CYCLE_TESTABLE, when set, must agree with the SO. The SATB companion
# (test_generation_satb_obligations.cpp) only exists in the testable
# configuration: it reaches product internals through testable-only friend
# access, so with a default SO it is not built and SATB_RC=NOT_RUN.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit"
OUT="${GC_CYCLE_OUT:?set GC_CYCLE_OUT}"
LIB="${GCV2_RUNTIME_LIB_DIR:?set GCV2_RUNTIME_LIB_DIR to the product pair}"
mkdir -p "$OUT"
SDK="${CANGJIE_HOME:?set CANGJIE_HOME to matching compiler}"
HEADERS=$(python3 "$ROOT/runtime/build/resolve_runtime_headers.py" "$ROOT/runtime" "$LIB")
nm -D "$LIB/libcangjie-runtime.so" >"$OUT/runtime-dynamic-symbols.txt"
# Product shape from both remap-receipt endpoints, exactly as run_standalone.sh
# derives REMAP_RECEIPT_PRODUCT_SHAPE; a partial export is an invalid shape.
nm -D --defined-only "$LIB/libcangjie-runtime.so" | c++filt >"$OUT/runtime-defined-symbols.txt"
receipt_reset=0
receipt_read=0
if /usr/bin/grep -F -q 'MapleRuntime::ResetRemapYoungRootsTestReceipt(' "$OUT/runtime-defined-symbols.txt"; then
  receipt_reset=1
fi
if /usr/bin/grep -F -q 'MapleRuntime::ReadRemapYoungRootsTestReceipt(' "$OUT/runtime-defined-symbols.txt"; then
  receipt_read=1
fi
if [[ "$receipt_reset" -eq 1 && "$receipt_read" -eq 1 ]]; then
  SO_TESTABLE=1
elif [[ "$receipt_reset" -eq 0 && "$receipt_read" -eq 0 ]]; then
  SO_TESTABLE=0
else
  echo "GC_CYCLE_PRODUCT_SHAPE_INCOMPLETE reset=$receipt_reset read=$receipt_read so=$LIB/libcangjie-runtime.so" >&2
  exit 19
fi
SO_GC_UNIT_TESTS=0
if /usr/bin/grep -Eq 'CJ_MRT_SetLargeArrayInitTestHooks' "$OUT/runtime-dynamic-symbols.txt"; then
  SO_GC_UNIT_TESTS=1
fi
if [[ -n "${GC_CYCLE_TESTABLE:-}" && "$GC_CYCLE_TESTABLE" != "$SO_TESTABLE" ]]; then
  echo "GC_CYCLE_PRODUCT_CONFIGURATION_MISMATCH GC_CYCLE_TESTABLE=$GC_CYCLE_TESTABLE so_testable=$SO_TESTABLE so=$LIB/libcangjie-runtime.so" >&2
  exit 6
fi
TEST_FLAGS=()
SATB_ENABLED=0
if [[ "$SO_TESTABLE" == 1 ]]; then
  TEST_FLAGS+=(-DMRT_TESTABLE_INTERNALS=1 -DMRT_PRODUCT_TESTABLE_INTERNALS=1)
  SATB_ENABLED=1
fi
if [[ "$SO_GC_UNIT_TESTS" == 1 ]]; then
  TEST_FLAGS+=(-DMRT_GC_UNIT_TESTS=1)
fi
echo "GC_CYCLE_PRODUCT_CONFIGURATION testable=$SO_TESTABLE gc_unit_tests=$SO_GC_UNIT_TESTS flags=${TEST_FLAGS[*]:-none}"
if [[ "${GC_CYCLE_REUSE_ELFS:-0}" != 1 ]]; then
"${CXX:-clang++}" -shared -fPIC -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti \
  -fvisibility-inlines-hidden ${TEST_FLAGS[@]+"${TEST_FLAGS[@]}"} \
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
  -fvisibility-inlines-hidden ${TEST_FLAGS[@]+"${TEST_FLAGS[@]}"} \
  -I"$SRC" -I"$ROOT/runtime/src" -I"$ROOT/runtime/src/Heap" \
  -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include" \
  -I"$ROOT/runtime/include" -I"$HEADERS/include" \
  -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include" \
  "$SRC/gc_unit_main.cpp" "$SRC/gc_cycle_sequence_fixture.cpp" "$SRC/gc_worker_fixture.cpp" \
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
  # Same process model as run_standalone.sh/run_parallel_tests.sh: one process
  # per registered test, selected with --gtest_filter=, so gc_unit_main.cpp
  # binds CollectorProxy's built-in collector at each isolated entry. A
  # whole-suite process has no such binding before its first in-process test.
  SATB_ENV="LD_LIBRARY_PATH=$LIB:$SDK/runtime/lib/linux_x86_64_cjnative${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  env "$SATB_ENV" "$OUT/generation_satb_obligations" --gtest_list_tests >"$OUT/satb-tests.raw" 2>"$OUT/satb-tests.stderr"
  list_rc=$?
  mapfile -t satb_tests < <(awk '
    /^[A-Za-z_][A-Za-z0-9_]*\.$/ { suite = substr($0, 1, length($0) - 1); next }
    /^[[:space:]]+[A-Za-z_][A-Za-z0-9_]*$/ { sub(/^[[:space:]]+/, ""); print suite "." $0 }' "$OUT/satb-tests.raw")
  satb_total=${#satb_tests[@]}
  satb_failed=0
  if [[ "$list_rc" != 0 || "$satb_total" == 0 ]]; then
    echo "SATB_LIST_TESTS_FAIL rc=$list_rc tests=$satb_total" >&2
    satb_failed=1
  fi
  for test in ${satb_tests[@]+"${satb_tests[@]}"}; do
    env "$SATB_ENV" timeout 60s "$OUT/generation_satb_obligations" "--gtest_filter=$test" \
      >"$OUT/satb-$test.log" 2>&1
    test_rc=$?
    cat "$OUT/satb-$test.log"
    printf "SATB_TEST name=%s rc=%s\n" "$test" "$test_rc"
    [[ "$test_rc" == 0 ]] || satb_failed=$((satb_failed + 1))
  done
  printf "SATB_TESTS=%s SATB_FAILED=%s\n" "$satb_total" "$satb_failed"
  satb_rc=$satb_failed
fi
printf "CYCLE_RC=%s SATB_RC=%s\n" "$cycle_rc" "$satb_rc"
[[ "$cycle_rc" == 0 && ( "$satb_rc" == 0 || "$satb_rc" == NOT_RUN ) ]]
