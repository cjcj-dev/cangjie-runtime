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
unset CANGJIE_HOME CJC GCV2_RUNTIME_LIB_DIR GCV2_RUNTIME_CONFIG \
  GCV2_RUNTIME_OUTPUT_ROOT MRT_TESTABLE_INTERNALS \
  GC_UNIT_GATE_LANGUAGE_TESTS GC_UNIT_GATE_SKIP GC_UNIT_GATE_STATUS \
  GC_UNIT_OUT GC_UNIT_TALLY_FILE
# Synthetic gate arms likewise supply their own header root. The copied-pair
# integration regression exercises automatic publication lookup with real SOs.
export GC_UNIT_CJC_RUNTIME_LIB_DIR="$fixture/lib"
export GCV2_RUNTIME_OUTPUT_ROOT="$fixture/selected-output"
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
printf '#!/usr/bin/env bash\nheader_root=${GC_UNIT_OHOS_HEADER_ROOT_TOKEN:-${GCV2_RUNTIME_OUTPUT_ROOT:?}/include}\nreceipt=${GC_UNIT_OHOS_HOST_RECEIPT:-${GC_UNIT_OUT:?}/ohos_host.receipt}\necho "GC_UNIT_OHOS_HOST_HEADER_ROOT=$header_root"\nprintf "RESULT=PASS\\nFILTER_MAJOR=PASS\\nFILTER_POST=PASS\\nFILTER_EMPTY=PASS\\n" >"$receipt"\nexit 0\n' \
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
# Reusing an explicitly supplied ELF does not compile headers. The runner's
# receipt remains mandatory, but a fresh-compilation record is not required.
PATH="$fixture/bin:$PATH" GC_UNIT_GATE_CONTRACT_SELFTEST=1 MRT_GC_UNIT_OHOS_HOST=1 \
  GCV2_RUNTIME_OUTPUT_ROOT="$ohos_output_root" GCV2_RUNTIME_LIB_DIR="$fixture/lib" \
  GC_UNIT_OHOS_HOST_TEST_ELF="$fixture/reused-elf" \
  GC_UNIT_OHOS_HEADER_ROOT_TOKEN=not-a-fresh-compile \
  GC_UNIT_OUT="$fixture/ohos-reused-out" GC_UNIT_GATE_STATUS="$fixture/ohos-reused.status" \
  bash "$fixture/runtime/tests/gc_unit/gate_gc_unit.sh" >"$fixture/ohos-reused.log" 2>&1
/usr/bin/grep -qx 'GATE=PASS' "$fixture/ohos-reused.status"
echo 'OHOS explicit ELF reuse: rc=0 receipt=PASS'
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
  "RUNTIME_SHA256=$(sha256sum "$config_lib/libcangjie-runtime.so" | awk '{print $1}')" \
  "BOUNDSCHECK_SHA256=$(sha256sum "$config_lib/libboundscheck.so" | awk '{print $1}')" \
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
printf '#!/usr/bin/env bash\necho "00000000 T _ZNK12MapleRuntime13RegionManager25PendingStalledAllocationsEv@@CANGJIE"\n' >"$fixture/bin/nm"
printf '#!/usr/bin/env bash\nexit 0\n' >"$fixture/sdk/bin/cjc"
chmod +x "$fixture/sdk/bin/cjc"
mkdir -p "$fixture/sdk/third_party/llvm/bin" "$fixture/sdk/lib/linux_x86_64_cjnative"
printf 'fixture llc\n' >"$fixture/sdk/third_party/llvm/bin/llc"
printf 'fixture opt\n' >"$fixture/sdk/third_party/llvm/bin/opt"
printf 'fixture std\n' >"$fixture/sdk/lib/linux_x86_64_cjnative/libcangjie-std-core.a"
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
echo "LANGUAGE_ENTRY_ASSERT mode=only trace=$(paste -sd, "$fixture/only.trace")"
[[ "$(cat "$fixture/only.trace")" == $'FINALIZER_TRIGGER\nPHASE_ENTRY_TRIGGER\nSEGMENTED_ARRAY_MANAGED' ]]
/usr/bin/grep -qx 'LANGUAGE_TESTS=LANGUAGE_DONE' "$fixture/only.status"

PATH="$fixture/bin:$PATH" GC_UNIT_GATE_TRACE="$fixture/all.trace" GC_UNIT_OUT="$fixture/all-out" \
  GC_UNIT_GATE_CONTRACT_SELFTEST=1 CANGJIE_HOME="$fixture/sdk" CJC="$fixture/sdk/bin/cjc" \
  GCV2_RUNTIME_LIB_DIR="$fixture/lib" GC_UNIT_GATE_STATUS="$fixture/all.status" \
  bash "$fixture/runtime/tests/gc_unit/gate_gc_unit.sh" >"$fixture/all.log" 2>&1
[[ "$(cat "$fixture/all.trace")" == $'CPP_SUITE\nFINALIZER_TRIGGER\nPHASE_ENTRY_TRIGGER\nSEGMENTED_ARRAY_MANAGED' ]]
/usr/bin/grep -qx 'LANGUAGE_TESTS=LANGUAGE_DONE' "$fixture/all.status"

# Missing compilers must fail both C++ completion paths. A stamped C++ PASS
# does not supply language evidence, even when it came from a full run.
for source in fresh cache; do
  out="$fixture/no-cjc-$source-out"
  mkdir -p "$out"
  if [[ "$source" == cache ]]; then
    touch "$out/.gate_stamp"
  fi
  set +e
  PATH="$fixture/bin:$PATH" CJC="$fixture/missing-cjc" GC_UNIT_GATE_TRACE="$fixture/no-cjc-$source.trace" \
    GC_UNIT_OUT="$out" GC_UNIT_GATE_CONTRACT_SELFTEST=1 \
    GCV2_RUNTIME_LIB_DIR="$fixture/lib" GC_UNIT_GATE_STATUS="$fixture/no-cjc-$source.status" \
    bash "$fixture/runtime/tests/gc_unit/gate_gc_unit.sh" >"$fixture/no-cjc-$source.log" 2>&1
  missing_rc=$?
  set -e
  echo "LANGUAGE_MISSING_ASSERT source=$source rc=$missing_rc"
  [[ $missing_rc -eq 2 ]]
  /usr/bin/grep -qx 'GATE=FAIL' "$fixture/no-cjc-$source.status"
  /usr/bin/grep -qx 'REASON=NO_CJC' "$fixture/no-cjc-$source.status"
  /usr/bin/grep -qx "CPP_SUITE_SOURCE=${source^^}" "$fixture/no-cjc-$source.status"
done

# The identity must describe the components actually selected, and changing
# any embedded compiler/std input must execute the language consumers again.
for component in cjc llc opt std; do
  case "$component" in
    cjc) file=bin/cjc; key=CJC_SHA256 ;;
    llc) file=third_party/llvm/bin/llc; key=LLC_SHA256 ;;
    opt) file=third_party/llvm/bin/opt; key=OPT_SHA256 ;;
    std) file=lib/linux_x86_64_cjnative/libcangjie-std-core.a; key=STD_SHA256 ;;
  esac
  previous=$(sed -n "s/^$key=//p" "$fixture/all.status")
  printf '\n# identity change\n' >>"$fixture/sdk/$file"
  rm -f "$fixture/all.trace"
  PATH="$fixture/bin:$PATH" GC_UNIT_GATE_TRACE="$fixture/all.trace" GC_UNIT_OUT="$fixture/all-out" \
    GC_UNIT_GATE_CONTRACT_SELFTEST=1 CANGJIE_HOME="$fixture/sdk" CJC="$fixture/sdk/bin/cjc" \
    GCV2_RUNTIME_LIB_DIR="$fixture/lib" GC_UNIT_GATE_STATUS="$fixture/all.status" \
    bash "$fixture/runtime/tests/gc_unit/gate_gc_unit.sh" >"$fixture/all-$component.log" 2>&1
  actual=$(sed -n "s/^$key=//p" "$fixture/all.status")
  echo "LANGUAGE_IDENTITY_ASSERT component=$component previous=$previous actual=$actual"
  [[ "$actual" =~ ^[0-9a-f]{64}$ && "$actual" != "$previous" ]]
  if [[ "$component" != std ]]; then
    [[ "$actual" == "$(sha256sum "$fixture/sdk/$file" | awk '{print $1}')" ]]
  fi
  echo "LANGUAGE_REEXEC_ASSERT component=$component trace=$(test ! -f "$fixture/all.trace" || paste -sd, "$fixture/all.trace")"
  [[ "$(cat "$fixture/all.trace" 2>/dev/null || true)" == $'FINALIZER_TRIGGER\nPHASE_ENTRY_TRIGGER\nSEGMENTED_ARRAY_MANAGED' ]]
  /usr/bin/grep -qx 'CPP_SUITE_SOURCE=CACHE' "$fixture/all.status"
  /usr/bin/grep -qx 'LANGUAGE_TESTS=LANGUAGE_DONE' "$fixture/all.status"
done
rm -f "$fixture/all.trace"
PATH="$fixture/bin:$PATH" GC_UNIT_GATE_TRACE="$fixture/all.trace" GC_UNIT_OUT="$fixture/all-out" \
  GC_UNIT_GATE_CONTRACT_SELFTEST=1 CANGJIE_HOME="$fixture/sdk" CJC="$fixture/sdk/bin/cjc" \
  GCV2_RUNTIME_LIB_DIR="$fixture/lib" GC_UNIT_GATE_STATUS="$fixture/all.status" \
  bash "$fixture/runtime/tests/gc_unit/gate_gc_unit.sh" >"$fixture/all-cached.log" 2>&1
[[ ! -f "$fixture/all.trace" ]]
/usr/bin/grep -qx 'REASON=CACHED_PASS' "$fixture/all.status"
echo 'LANGUAGE_CACHE_ASSERT unchanged=cache changed=fresh'

# Identity collection must not allow an incomplete SDK or a missing compiler
# host to reach a managed consumer. Both all and only use the same binding.
for mode in all only; do
  set +e
  PATH="$fixture/bin:$PATH" GC_UNIT_GATE_CONTRACT_SELFTEST=1 \
    GC_UNIT_GATE_LANGUAGE_TESTS="$mode" GC_UNIT_OUT="$fixture/host-missing-$mode" \
    CANGJIE_HOME="$fixture/sdk" CJC="$fixture/sdk/bin/cjc" \
    GC_UNIT_CJC_RUNTIME_LIB_DIR="$fixture/missing-host" \
    GCV2_RUNTIME_LIB_DIR="$fixture/lib" GC_UNIT_GATE_STATUS="$fixture/host-missing-$mode.status" \
    bash "$fixture/runtime/tests/gc_unit/gate_gc_unit.sh" >"$fixture/host-missing-$mode.log" 2>&1
  host_rc=$?
  set -e
  echo "LANGUAGE_HOST_ASSERT mode=$mode rc=$host_rc"
  [[ "$host_rc" -eq 2 ]]
  /usr/bin/grep -qx 'REASON=LANGUAGE_HOST_RUNTIME_MISSING' "$fixture/host-missing-$mode.status"
done
