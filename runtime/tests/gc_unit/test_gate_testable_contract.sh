#!/usr/bin/env bash
# Gate contract fixture: TESTABLE=1 must not hide a missing managed fixture or
# a missing product hook behind the NO_CJC path.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
fixture="$(mktemp -d /tmp/gc-unit-gate-contract.XXXXXX)"
trap 'rm -rf "$fixture"' EXIT
mkdir -p "$fixture/runtime/tests/gc_unit" "$fixture/runtime/src" "$fixture/lib" "$fixture/bin"
cp "$ROOT/runtime/tests/gc_unit/gate_gc_unit.sh" "$fixture/runtime/tests/gc_unit/"
for f in run_standalone.sh run_finalizer_trigger.sh run_phase_entry_trigger.sh; do
  printf '#!/usr/bin/env bash\n# test_x.cpp\nexit 0\n' >"$fixture/runtime/tests/gc_unit/$f"
  chmod +x "$fixture/runtime/tests/gc_unit/$f"
done
printf 'int main() {}\n' >"$fixture/runtime/tests/gc_unit/test_defect_regressions.cpp"
printf 'placeholder\n' >"$fixture/runtime/tests/gc_unit/finalizer_trigger.cj"
printf 'placeholder\n' >"$fixture/runtime/tests/gc_unit/phase_entry_trigger.cj"
printf 'placeholder\n' >"$fixture/runtime/tests/gc_unit/phase_entry_major.cj"
printf 'test_x.cpp\n' >"$fixture/runtime/tests/gc_unit/CMakeLists.txt"
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
