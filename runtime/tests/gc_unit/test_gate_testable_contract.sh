#!/usr/bin/env bash
# Gate contract fixture: TESTABLE=1 must not hide a missing managed fixture or
# a missing product hook behind the NO_CJC path.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
PYTHONDONTWRITEBYTECODE=1 python3 "$ROOT/runtime/tests/gc_unit/test_mutualwait_ast_once.py"
echo "MUTUALWAIT_AST_ONCE_CONTRACT_OK"
fixture="$(mktemp -d /tmp/gc-unit-gate-contract.XXXXXX)"
trap 'rm -rf "$fixture"' EXIT

# Exercise the real run_standalone.sh call site, not only the receipt helper.
# Manifest-only mode stops immediately after that call site, keeping this
# contract independent from the large C++ link while still detecting a deleted
# or bypassed invocation in the production runner.
mw_fixture="$fixture/mutualwait"
mkdir -p "$mw_fixture/runtime/tests/gc_unit" "$mw_fixture/runtime/src/Heap" \
  "$mw_fixture/runtime/include" "$mw_fixture/lib" "$mw_fixture/bin"
cp "$ROOT/runtime/tests/gc_unit/run_standalone.sh" \
  "$ROOT/runtime/tests/gc_unit/run_mutualwait_manifest.py" \
  "$mw_fixture/runtime/tests/gc_unit/"
printf 'int mutualwait_source;\n' >"$mw_fixture/runtime/tests/gc_unit/clear_entries_product_unit.cpp"
printf 'manifest\n' >"$mw_fixture/runtime/tests/gc_unit/product_call_manifest_mutualwait.tsv"
printf 'int product_anchor;\n' >"$mw_fixture/runtime/src/Heap/anchor.cpp"
printf '#!/usr/bin/env bash\nexit 0\n' >"$mw_fixture/bin/nm"
printf '#!/usr/bin/env bash\nprintf "%%s\\n" "${CJRT_HEAP_FILLER:-default}" >>"${MUTUALWAIT_FIXTURE_ARM_TRACE:?}"\necho mutualwait-fixture-compiler-v1\n' >"$mw_fixture/bin/cxx"
printf '#!/usr/bin/env python3\nimport os\nfrom pathlib import Path\np=Path(os.environ["MUTUALWAIT_FIXTURE_COUNT"])\nwith p.open("a") as f: f.write("run\\n")\nfor i in range(7): print(f"GATE_MUTUALWAIT_PRODUCT_MANIFEST_ROW_OK row={i}")\n' \
  >"$mw_fixture/runtime/tests/gc_unit/check_mutualwait_manifest.py"
chmod +x "$mw_fixture/bin/nm" "$mw_fixture/bin/cxx" \
  "$mw_fixture/runtime/tests/gc_unit/check_mutualwait_manifest.py"
touch "$mw_fixture/lib/libcangjie-runtime.so"
mw_count="$mw_fixture/analyzer.count"
mw_arm_trace="$mw_fixture/arm.trace"
mw_out="$mw_fixture/out"
PATH="$mw_fixture/bin:$PATH" CXX="$mw_fixture/bin/cxx" CJRT_HEAP_FILLER=default \
  MUTUALWAIT_FIXTURE_COUNT="$mw_count" GCV2_RUNTIME_LIB_DIR="$mw_fixture/lib" \
  MUTUALWAIT_FIXTURE_ARM_TRACE="$mw_arm_trace" \
  GC_UNIT_OUT="$mw_out" GC_UNIT_MUTUALWAIT_MANIFEST_ONLY=1 \
  bash "$mw_fixture/runtime/tests/gc_unit/run_standalone.sh" >"$mw_fixture/default.log" 2>&1 && \
  mw_default_rc=0 || mw_default_rc=$?
PATH="$mw_fixture/bin:$PATH" CXX="$mw_fixture/bin/cxx" CJRT_HEAP_FILLER=0 \
  MUTUALWAIT_FIXTURE_COUNT="$mw_count" GCV2_RUNTIME_LIB_DIR="$mw_fixture/lib" \
  MUTUALWAIT_FIXTURE_ARM_TRACE="$mw_arm_trace" \
  GC_UNIT_OUT="$mw_out" GC_UNIT_MUTUALWAIT_MANIFEST_ONLY=1 \
  bash "$mw_fixture/runtime/tests/gc_unit/run_standalone.sh" >"$mw_fixture/filler.log" 2>&1 && \
  mw_filler_rc=0 || mw_filler_rc=$?
mw_invocations=0
if [[ -f "$mw_count" ]]; then
  mw_invocations=$(wc -l <"$mw_count")
fi
mw_rows=$(/usr/bin/grep -c GATE_MUTUALWAIT_PRODUCT_MANIFEST_ROW_OK "$mw_fixture/default.log" || true)
mw_default_entries=$(/usr/bin/grep -c '^default$' "$mw_arm_trace" || true)
mw_filler_entries=$(/usr/bin/grep -c '^0$' "$mw_arm_trace" || true)
printf 'MUTUALWAIT_RUN_STANDALONE_PAIR_ASSERT default_rc=%s filler_rc=%s analyzer_invocations=%s rows=%s default_entries=%s filler_entries=%s\n' \
  "$mw_default_rc" "$mw_filler_rc" "$mw_invocations" "$mw_rows" \
  "$mw_default_entries" "$mw_filler_entries"
[[ "$mw_default_rc" -eq 0 && "$mw_filler_rc" -eq 0 && "$mw_invocations" -eq 1 && \
   "$mw_rows" -eq 7 && "$mw_default_entries" -eq 1 && "$mw_filler_entries" -eq 1 ]]
echo "MUTUALWAIT_RUN_STANDALONE_PAIR_OK"

# The parent gate supplies its own compiler, runtime, status, mode, and skip
# controls.  Each fixture arm below owns all of those inputs; inheriting even
# one can turn a negative arm into a false PASS.
unset CANGJIE_HOME CJC GCV2_RUNTIME_LIB_DIR GCV2_RUNTIME_CONFIG \
  GCV2_RUNTIME_OUTPUT_ROOT MRT_TESTABLE_INTERNALS \
  GC_UNIT_GATE_LANGUAGE_TESTS GC_UNIT_GATE_SKIP GC_UNIT_GATE_STATUS \
  GC_UNIT_OUT GC_UNIT_TALLY_FILE
mkdir -p "$fixture/runtime/tests/gc_unit" "$fixture/runtime/src" "$fixture/runtime/build" \
  "$fixture/lib" "$fixture/bin" \
  "$fixture/sdk/bin"
cp "$ROOT/runtime/tests/gc_unit/gate_gc_unit.sh" "$fixture/runtime/tests/gc_unit/"
cp "$ROOT/runtime/build/resolve_runtime_output.sh" "$fixture/runtime/build/"
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
printf 'placeholder\n' >"$fixture/lib/libboundscheck.so"
printf '#!/usr/bin/env bash\nexit 0\n' >"$fixture/bin/nm"
chmod +x "$fixture/bin/nm"

# The OHOS-host compiler must consume generated headers from the same output
# identity as the selected runtime SO.  Keep the runner otherwise successful
# in the negative arm so the exact path assertion, rather than an earlier
# compilation or receipt failure, is what turns red.
cp "$fixture/runtime/tests/gc_unit/run_standalone.sh" "$fixture/default-runner.sh"
printf '#!/usr/bin/env bash\nheader_root=${GC_UNIT_OHOS_HEADER_ROOT_TOKEN:-${GCV2_RUNTIME_OUTPUT_ROOT:?}/include}\necho "GC_UNIT_OHOS_HOST_HEADER_ROOT=$header_root"\nprintf "RESULT=PASS\\nFILTER_MAJOR=PASS\\nFILTER_POST=PASS\\nFILTER_EMPTY=PASS\\n" >"${GC_UNIT_OHOS_HOST_RECEIPT:?}"\nexit 0\n' \
  >"$fixture/runtime/tests/gc_unit/run_standalone.sh"
chmod +x "$fixture/runtime/tests/gc_unit/run_standalone.sh"
ohos_output_root="$fixture/selected-output"
mkdir -p "$ohos_output_root/include"
PATH="$fixture/bin:$PATH" GC_UNIT_GATE_CONTRACT_SELFTEST=1 MRT_GC_UNIT_OHOS_HOST=1 \
  GCV2_RUNTIME_OUTPUT_ROOT="$ohos_output_root" GCV2_RUNTIME_LIB_DIR="$fixture/lib" \
  GC_UNIT_OUT="$fixture/ohos-good-out" GC_UNIT_GATE_STATUS="$fixture/ohos-good.status" \
  bash "$fixture/runtime/tests/gc_unit/gate_gc_unit.sh" >"$fixture/ohos-good.log" 2>&1
set +e
PATH="$fixture/bin:$PATH" GC_UNIT_GATE_CONTRACT_SELFTEST=1 MRT_GC_UNIT_OHOS_HOST=1 \
  GCV2_RUNTIME_OUTPUT_ROOT="$ohos_output_root" GCV2_RUNTIME_LIB_DIR="$fixture/lib" \
  GC_UNIT_OHOS_HEADER_ROOT_TOKEN="$fixture/stale-output/include" \
  GC_UNIT_OUT="$fixture/ohos-mismatch-out" GC_UNIT_GATE_STATUS="$fixture/ohos-mismatch.status" \
  bash "$fixture/runtime/tests/gc_unit/gate_gc_unit.sh" >"$fixture/ohos-mismatch.log" 2>&1
ohos_mismatch_rc=$?
set -e
[[ $ohos_mismatch_rc -eq 1 ]]
/usr/bin/grep -qx 'REASON=OHOS_HOST_HEADER_ROOT_MISMATCH' "$fixture/ohos-mismatch.status"
/usr/bin/grep -q 'compiler did not use the selected configuration header root' \
  "$fixture/ohos-mismatch.log"
printf 'OHOS header identity: rc=0 mismatch_rc=%s root=%s/include\n' \
  "$ohos_mismatch_rc" "$ohos_output_root"
mv "$fixture/default-runner.sh" "$fixture/runtime/tests/gc_unit/run_standalone.sh"

# Configuration selection is a gate input, not a directory scan.  Prove the
# requested manifest is accepted and an explicit path from another
# configuration is rejected before either product can reach the suite.
config_id=linux-x86_64-release-default-111111111111
config_lib="$fixture/runtime/output/temp/$config_id/lib/x86_64_Release"
mkdir -p "$config_lib" "$fixture/other-lib"
printf 'runtime-configured\n' >"$config_lib/libcangjie-runtime.so"
printf 'bounds-configured\n' >"$config_lib/libboundscheck.so"
printf 'runtime-other\n' >"$fixture/other-lib/libcangjie-runtime.so"
printf '%s\n' \
  'SCHEMA_VERSION=1' \
  "CONFIG_ID=$config_id" \
  'CONFIG_SIGNATURE_SHA256=1111111111111111111111111111111111111111111111111111111111111111' \
  "LIB_DIR=$config_lib" >"$fixture/runtime/output/temp/$config_id/runtime-build-config.txt"

PATH="$fixture/bin:$PATH" GC_UNIT_GATE_TRACE="$fixture/config.trace" \
  GC_UNIT_OUT="$fixture/config-out" GC_UNIT_GATE_CONTRACT_SELFTEST=1 \
  GC_UNIT_GATE_LANGUAGE_TESTS=defer GCV2_RUNTIME_CONFIG="$config_id" \
  GCV2_RUNTIME_LIB_DIR="$config_lib" \
  bash "$fixture/runtime/tests/gc_unit/gate_gc_unit.sh" >"$fixture/config.log" 2>&1
config_status="$config_lib/gc_unit_gate.status"
if [[ ! -f "$config_status" ]]; then
  echo "CONFIG_STATUS_LOCATION_FAIL: expected status beside selected runtime: $config_status" >&2
  exit 1
fi
/usr/bin/grep -qx "RUNTIME_CONFIG_ID=$config_id" "$config_status"
/usr/bin/grep -Eq '^RUNTIME_SHA256=[0-9a-f]{64}$' "$config_status"
/usr/bin/grep -Eq '^BOUNDSCHECK_SHA256=[0-9a-f]{64}$' "$config_status"
/usr/bin/grep -q "GC_UNIT_RUNTIME_IDENTITY config=$config_id" "$fixture/config.log"

set +e
PATH="$fixture/bin:$PATH" GC_UNIT_GATE_CONTRACT_SELFTEST=1 \
  GC_UNIT_GATE_LANGUAGE_TESTS=defer GCV2_RUNTIME_CONFIG="$config_id" \
  GCV2_RUNTIME_LIB_DIR="$fixture/other-lib" GC_UNIT_GATE_STATUS="$fixture/config-mismatch.status" \
  bash "$fixture/runtime/tests/gc_unit/gate_gc_unit.sh" >"$fixture/config-mismatch.log" 2>&1
config_mismatch_rc=$?
set -e
[[ $config_mismatch_rc -eq 2 ]]
/usr/bin/grep -qx 'REASON=RUNTIME_CONFIG_MISMATCH' "$fixture/config-mismatch.status"
/usr/bin/grep -q 'explicit library directory does not match' "$fixture/config-mismatch.log"
printf 'CONFIG selection: rc=0 mismatch_rc=%s id=%s\n' "$config_mismatch_rc" "$config_id"

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
