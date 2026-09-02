#!/usr/bin/env bash
# Gate contract fixture: TESTABLE=1 must not hide a missing managed fixture or
# a missing product hook behind the NO_CJC path.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
fixture="$(mktemp -d /tmp/gc-unit-gate-contract.XXXXXX)"
trap 'rm -rf "$fixture"' EXIT
# The parent gate supplies its own compiler, runtime, status, mode, and skip
# controls.  Each fixture arm below owns all of those inputs; inheriting even
# one can turn a negative arm into a false PASS.
unset CANGJIE_HOME CJC GCV2_RUNTIME_LIB_DIR MRT_TESTABLE_INTERNALS \
  GC_UNIT_GATE_LANGUAGE_TESTS GC_UNIT_GATE_SKIP GC_UNIT_GATE_STATUS \
  GC_UNIT_OUT GC_UNIT_TALLY_FILE
mkdir -p "$fixture/runtime/tests/gc_unit" "$fixture/runtime/src" "$fixture/lib" "$fixture/bin" \
  "$fixture/sdk/bin"
cp "$ROOT/runtime/tests/gc_unit/gate_gc_unit.sh" "$fixture/runtime/tests/gc_unit/"
printf '#!/usr/bin/env bash\n# test_x.cpp\necho CPP_SUITE >>"${GC_UNIT_GATE_TRACE:?}"\nmkdir -p "$(dirname "${GC_UNIT_TALLY_FILE:?}")"\necho "[========] 1 tests: 1 passed, 0 failed" >"$GC_UNIT_TALLY_FILE"\nexit 0\n' >"$fixture/runtime/tests/gc_unit/run_standalone.sh"
printf '#!/usr/bin/env bash\necho FINALIZER_TRIGGER >>"${GC_UNIT_GATE_TRACE:?}"\nexit 0\n' >"$fixture/runtime/tests/gc_unit/run_finalizer_trigger.sh"
printf '#!/usr/bin/env bash\necho PHASE_ENTRY_TRIGGER >>"${GC_UNIT_GATE_TRACE:?}"\nexit 0\n' >"$fixture/runtime/tests/gc_unit/run_phase_entry_trigger.sh"
chmod +x "$fixture/runtime/tests/gc_unit/run_standalone.sh" \
  "$fixture/runtime/tests/gc_unit/run_finalizer_trigger.sh" \
  "$fixture/runtime/tests/gc_unit/run_phase_entry_trigger.sh"
printf 'int main() {}\n' >"$fixture/runtime/tests/gc_unit/test_defect_regressions.cpp"
printf 'placeholder\n' >"$fixture/runtime/tests/gc_unit/finalizer_trigger.cj"
printf 'placeholder\n' >"$fixture/runtime/tests/gc_unit/phase_entry_trigger.cj"
printf 'placeholder\n' >"$fixture/runtime/tests/gc_unit/phase_entry_major.cj"
printf 'test_x.cpp\n' >"$fixture/runtime/tests/gc_unit/CMakeLists.txt"
touch "$fixture/runtime/tests/gc_unit/known_failures.txt"
printf 'placeholder\n' >"$fixture/lib/libcangjie-runtime.so"
printf '#!/usr/bin/env bash\nexit 0\n' >"$fixture/bin/nm"
chmod +x "$fixture/bin/nm"

set +e
PATH="$fixture/bin:$PATH" CJC=/nonexistent MRT_TESTABLE_INTERNALS=1 \
  GC_UNIT_GATE_CONTRACT_SELFTEST=1 GC_UNIT_GATE_SKIP=0 \
  GCV2_RUNTIME_LIB_DIR="$fixture/lib" GC_UNIT_GATE_STATUS="$fixture/testable-missing.status" \
  bash "$fixture/runtime/tests/gc_unit/gate_gc_unit.sh" >"$fixture/testable-missing.log" 2>&1
testable_missing_rc=$?
PATH="$fixture/bin:$PATH" CJC=/nonexistent MRT_TESTABLE_INTERNALS=0 GC_UNIT_GATE_SKIP=1 \
  GC_UNIT_GATE_CONTRACT_SELFTEST=1 \
  GCV2_RUNTIME_LIB_DIR="$fixture/lib" GC_UNIT_GATE_STATUS="$fixture/default-no-cjc.status" \
  bash "$fixture/runtime/tests/gc_unit/gate_gc_unit.sh" >"$fixture/default-no-cjc.log" 2>&1
default_no_cjc_rc=$?
set -e

testable_missing_reason=$(sed -n 's/^REASON=//p' "$fixture/testable-missing.status")
default_no_cjc_reason=$(sed -n 's/^REASON=//p' "$fixture/default-no-cjc.status")
printf 'TESTABLE=1 missing fixture: rc=%s reason=%s\n' "$testable_missing_rc" "$testable_missing_reason"
printf 'TESTABLE=0 explicit-skip control: rc=%s reason=%s\n' "$default_no_cjc_rc" "$default_no_cjc_reason"

[[ "$testable_missing_rc" -eq 2 && "$testable_missing_reason" == STARTED ]]
grep -q 'missing managed segmented-array language-level test' "$fixture/testable-missing.log"
[[ "$default_no_cjc_rc" -eq 0 && "$default_no_cjc_reason" == EXPLICIT_SKIP ]]

# Second negative arm: once the managed fixture exists, TESTABLE=1 still must
# reject a product SO that lacks the hook it promises.
printf '#!/usr/bin/env bash\nexit 0\n' >"$fixture/runtime/tests/gc_unit/run_segmented_array_managed.sh"
chmod +x "$fixture/runtime/tests/gc_unit/run_segmented_array_managed.sh"
printf 'main(): Int64 { return 0 }\n' >"$fixture/runtime/tests/gc_unit/segmented_array_managed.cj"
set +e
PATH="$fixture/bin:$PATH" CJC=/nonexistent MRT_TESTABLE_INTERNALS=1 \
  GC_UNIT_GATE_CONTRACT_SELFTEST=1 GC_UNIT_GATE_SKIP=0 \
  GCV2_RUNTIME_LIB_DIR="$fixture/lib" GC_UNIT_GATE_STATUS="$fixture/testable-hook-missing.status" \
  bash "$fixture/runtime/tests/gc_unit/gate_gc_unit.sh" >"$fixture/testable-hook-missing.log" 2>&1
testable_hook_missing_rc=$?
set -e
testable_hook_missing_reason=$(sed -n 's/^REASON=//p' "$fixture/testable-hook-missing.status")
printf 'TESTABLE=1 missing hook: rc=%s reason=%s\n' "$testable_hook_missing_rc" "$testable_hook_missing_reason"
[[ "$testable_hook_missing_rc" -eq 2 && "$testable_hook_missing_reason" == STARTED ]]
grep -q 'TESTABLE_INTERNALS=1 but product SO lacks segmented-array test hooks' "$fixture/testable-hook-missing.log"

# Mode matrix: defer must execute only C++, only must execute only the language
# entries, and the unset/default mode must retain the combined behavior.
printf '#!/usr/bin/env bash\necho "00000000 T CJ_MRT_SetLargeArrayInitTestHooks@@CANGJIE"\n' >"$fixture/bin/nm"
printf '#!/usr/bin/env bash\nexit 0\n' >"$fixture/sdk/bin/cjc"
chmod +x "$fixture/sdk/bin/cjc"
printf '#!/usr/bin/env bash\necho SEGMENTED_ARRAY_MANAGED >>"${GC_UNIT_GATE_TRACE:?}"\nexit 0\n' \
  >"$fixture/runtime/tests/gc_unit/run_segmented_array_managed.sh"
chmod +x "$fixture/runtime/tests/gc_unit/run_segmented_array_managed.sh"
touch "$fixture/lib/libcangjie-runtime.so"

PATH="$fixture/bin:$PATH" GC_UNIT_GATE_TRACE="$fixture/defer.trace" GC_UNIT_OUT="$fixture/defer-out" \
  GC_UNIT_GATE_CONTRACT_SELFTEST=1 GC_UNIT_GATE_LANGUAGE_TESTS=defer \
  GCV2_RUNTIME_LIB_DIR="$fixture/lib" GC_UNIT_GATE_STATUS="$fixture/defer.status" \
  bash "$fixture/runtime/tests/gc_unit/gate_gc_unit.sh" >"$fixture/defer.log" 2>&1
[[ "$(cat "$fixture/defer.trace")" == CPP_SUITE ]]
/usr/bin/grep -qx 'LANGUAGE_TESTS=LANGUAGE_DEFERRED' "$fixture/defer.status"

set +e
PATH="$fixture/bin:$PATH" GC_UNIT_GATE_TRACE="$fixture/only-missing.trace" \
  GC_UNIT_OUT="$fixture/only-missing-out" GC_UNIT_GATE_CONTRACT_SELFTEST=1 \
  GC_UNIT_GATE_LANGUAGE_TESTS=only GCV2_RUNTIME_LIB_DIR="$fixture/lib" \
  GC_UNIT_GATE_STATUS="$fixture/only-missing.status" \
  bash "$fixture/runtime/tests/gc_unit/gate_gc_unit.sh" >"$fixture/only-missing.log" 2>&1
only_missing_rc=$?
set -e
[[ "$only_missing_rc" -eq 2 ]]
/usr/bin/grep -qx 'REASON=LANGUAGE_SDK_MISSING' "$fixture/only-missing.status"

PATH="$fixture/bin:$PATH" GC_UNIT_GATE_TRACE="$fixture/only.trace" GC_UNIT_OUT="$fixture/only-out" \
  GC_UNIT_GATE_CONTRACT_SELFTEST=1 GC_UNIT_GATE_LANGUAGE_TESTS=only \
  CANGJIE_HOME="$fixture/sdk" CJC="$fixture/sdk/bin/cjc" \
  GCV2_RUNTIME_LIB_DIR="$fixture/lib" GC_UNIT_GATE_STATUS="$fixture/only.status" \
  bash "$fixture/runtime/tests/gc_unit/gate_gc_unit.sh" >"$fixture/only.log" 2>&1
[[ "$(cat "$fixture/only.trace")" == $'FINALIZER_TRIGGER\nPHASE_ENTRY_TRIGGER\nSEGMENTED_ARRAY_MANAGED' ]]
/usr/bin/grep -qx 'LANGUAGE_TESTS=LANGUAGE_DONE' "$fixture/only.status"

PATH="$fixture/bin:$PATH" GC_UNIT_GATE_TRACE="$fixture/all.trace" GC_UNIT_OUT="$fixture/all-out" \
  GC_UNIT_GATE_CONTRACT_SELFTEST=1 CANGJIE_HOME="$fixture/sdk" CJC="$fixture/sdk/bin/cjc" \
  GCV2_RUNTIME_LIB_DIR="$fixture/lib" GC_UNIT_GATE_STATUS="$fixture/all.status" \
  bash "$fixture/runtime/tests/gc_unit/gate_gc_unit.sh" >"$fixture/all.log" 2>&1
[[ "$(cat "$fixture/all.trace")" == $'CPP_SUITE\nFINALIZER_TRIGGER\nPHASE_ENTRY_TRIGGER\nSEGMENTED_ARRAY_MANAGED' ]]
/usr/bin/grep -qx 'LANGUAGE_TESTS=LANGUAGE_DONE' "$fixture/all.status"
