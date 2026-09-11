#!/usr/bin/env bash
# Standalone build+run of GC unit tests.
# Links product libcangjie-runtime so U3–U7 call product symbols (not models).
# Usage:
#   GCV2_RUNTIME_LIB_DIR=/path/to/lib/x86_64_Release bash runtime/tests/gc_unit/run_standalone.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit"
OUT="${GC_UNIT_OUT:-$ROOT/runtime/tests/gc_unit/build_standalone}"
CXX="${CXX:-clang++}"

# Mutual-wait receipts have an independent, fixed target set.  Do not derive
# it from the ProductFindToVersion calls that happen to remain in the source:
# deleting a test/call must shrink neither the manifest nor this guard.
MUTUALWAIT_MANIFEST="$SRC/product_call_manifest_mutualwait.tsv"
MUTUALWAIT_ANALYZER="$SRC/check_mutualwait_manifest.py"
MUTUALWAIT_RUNNER="$SRC/run_mutualwait_manifest.py"
MUTUALWAIT_SOURCE="$SRC/clear_entries_product_unit.cpp"
EXPECTED_MUTUALWAIT_TESTS=(
  ForwardingPublicationProduct.KeptActiveReceiptRemainsRequiredAfterTableRetires
  ForwardingPublicationProduct.ExemptPreservesRetiredReceiptAcrossActiveGeneration
  ForwardingPublicationProduct.ReclaimRetiredDefersResidualUntilActiveReceipt
  ForwardingPublicationProduct.ReclaimRetiredPreservesNewActiveReceiptHeader
  ForwardingPublicationProduct.FinishIncompleteUnmovablePublishesIdentityBeforeDone
  ForwardingPublicationProduct.FinishIncompleteNonFromResidualPublishesIdentityBeforeDone
  ForwardingPublicationProduct.ExemptRejectsForwardedWithoutAnyReceipt
)

validate_mutualwait_manifest() {
  local test_name compile_arg arm
  local analyzer_args=(
    --source "$MUTUALWAIT_SOURCE"
    --manifest "$MUTUALWAIT_MANIFEST"
    --product-root "$ROOT/runtime/src/Heap"
    --compiler "$CXX"
  )
  for test_name in "${EXPECTED_MUTUALWAIT_TESTS[@]}"; do
    analyzer_args+=(--expected-test "$test_name")
  done
  for compile_arg in \
      -std=gnu++17 -fno-rtti -fvisibility-inlines-hidden \
      "${TEST_DEFINES[@]}" -DMRT_TESTABLE_INTERNALS=1 \
      "${PUBLICATION_TESTABLE_FLAGS[@]}" "${INC_FLAGS[@]}"; do
    analyzer_args+=("--compile-arg=$compile_arg")
  done
  arm=default
  if [[ "${CJRT_HEAP_FILLER:-}" == "0" ]]; then
    arm=filler
  fi
  python3 "$MUTUALWAIT_RUNNER" \
    --arm "$arm" \
    --receipt "$OUT/.mutualwait_ast_receipt.json" \
    --analyzer "$MUTUALWAIT_ANALYZER" \
    -- "${analyzer_args[@]}" || return 11
}

mkdir -p "$OUT"

RUNTIME_LIB_DIR="${GCV2_RUNTIME_LIB_DIR:-}"
if [[ -z "$RUNTIME_LIB_DIR" || ! -f "$RUNTIME_LIB_DIR/libcangjie-runtime.so" ]]; then
  echo "error: set GCV2_RUNTIME_LIB_DIR to a dir containing libcangjie-runtime.so" >&2
  exit 2
fi

if [[ -z "${GCV2_RUNTIME_OUTPUT_ROOT:-}" ]]; then
  GCV2_RUNTIME_OUTPUT_ROOT=$(python3 "$ROOT/runtime/build/resolve_runtime_headers.py" \
    "$ROOT/runtime" "$RUNTIME_LIB_DIR")
fi

run_ohos_host_arm() {
  local so="$RUNTIME_LIB_DIR/libcangjie-runtime.so"
  local bounds="$RUNTIME_LIB_DIR/libboundscheck.so"
  local host_src="$SRC/ohos_host"
  local host_inc="$host_src/include"
  local elf="${GC_UNIT_OHOS_HOST_TEST_ELF:-$OUT/cj_gc_ohos_host_unit}"
  local runroot="$OUT/ohos_host_runroot"
  local receipt="${GC_UNIT_OHOS_HOST_RECEIPT:-$OUT/ohos_host.receipt}"
  local product_nm="$OUT/ohos_host_product.full-defined.txt"
  local test_nm="$OUT/ohos_host_test.full-defined.txt"
  local test_undef="$OUT/ohos_host_test.undefined.txt"
  local post_disassembly="$OUT/ohos_host_postresolve.disassembly.txt"
  local runtime_output_root="${GCV2_RUNTIME_OUTPUT_ROOT:-$(realpath -m "$RUNTIME_LIB_DIR/../..")}"
  local runtime_include_flags=(-I"$runtime_output_root/include")
  local libc_real
  local test_name key rc state
  local overall_rc=0

  rm -f "$receipt"
  if [[ ! -f "$bounds" || ! -f "$host_src/ohos_cycle_unit.cpp" ]]; then
    echo "GC_UNIT_OHOS_HOST_MISSING_INPUT runtime=$so bounds=$bounds source=$host_src/ohos_cycle_unit.cpp" >&2
    return 20
  fi

  nm --defined-only "$so" | c++filt >"$product_nm"
  for symbol in \
      'MRT_GC_UNIT_OHOS_HOST_RECEIPT' \
      'CJ_MRT_RolveCycleRef' \
      'MapleRuntime::WCollector::DoGarbageCollection()' \
      'MapleRuntime::WCollector::PostResolveCycleTask()'; do
    if ! /usr/bin/grep -F -q "$symbol" "$product_nm"; then
      echo "GC_UNIT_OHOS_HOST_PRODUCT_SYMBOL_MISSING symbol=$symbol" >&2
      return 21
    fi
  done

  mkdir -p "$OUT" "$runroot"
  libc_real="$($CXX -print-file-name=libc.so.6)"
  if [[ "$libc_real" == "libc.so.6" || ! -f "$libc_real" ]]; then
    echo "GC_UNIT_OHOS_HOST_LIBC_NOT_FOUND compiler=$CXX result=$libc_real" >&2
    return 22
  fi
  libc_real="$(readlink -f "$libc_real")"
  ln -sfn "$libc_real" "$runroot/libc.so"

  if [[ -z "${GC_UNIT_OHOS_HOST_TEST_ELF:-}" ]]; then
    echo "GC_UNIT_OHOS_HOST_HEADER_ROOT=${runtime_include_flags[0]#-I}"
    "$CXX" -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti -fexceptions \
      -fvisibility-inlines-hidden -D__OHOS__=1 -DMRT_GC_UNIT_TESTS=1 \
      -DMRT_TESTABLE_INTERNALS=1 -include string \
      -I"$host_inc" -I"$SRC" -I"$ROOT/runtime/src" -I"$ROOT/runtime/src/Heap" \
      -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include" \
      -I"$ROOT/runtime/include" \
      -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include" \
      "${runtime_include_flags[@]}" \
      "$SRC/gc_unit_main.cpp" "$host_src/ohos_cycle_unit.cpp" \
      -L"$RUNTIME_LIB_DIR" -Wl,-rpath,"$RUNTIME_LIB_DIR" -Wl,--exclude-libs,ALL \
      -lcangjie-runtime -lboundscheck -o "$elf"
  elif [[ ! -x "$elf" ]]; then
    echo "GC_UNIT_OHOS_HOST_REUSED_ELF_MISSING elf=$elf" >&2
    return 27
  else
    echo "GC_UNIT_OHOS_HOST_REUSING_ELF elf=$elf"
  fi

  # Full nm is deliberate: a local/weak copy in the test is still a second
  # implementation and must fail this product-identity guard.
  nm --defined-only "$elf" | c++filt >"$test_nm"
  nm -u "$elf" | c++filt >"$test_undef"
  if ! /usr/bin/grep -Eq '[[:space:]]main$' "$test_nm"; then
    echo "GC_UNIT_OHOS_HOST_NM_POSITIVE_CONTROL_FAIL symbol=main" >&2
    return 23
  fi
  for symbol in \
      'CJ_MRT_RolveCycleRef' \
      'MapleRuntime::WCollector::DoGarbageCollection()' \
      'MapleRuntime::WCollector::PostResolveCycleTask()'; do
    if /usr/bin/grep -F -q "$symbol" "$test_nm"; then
      echo "GC_UNIT_OHOS_HOST_LOCAL_PRODUCT_DEFINITION symbol=$symbol" >&2
      return 24
    fi
  done
  for symbol in \
      'MapleRuntime::WCollector::DoGarbageCollection()' \
      'MapleRuntime::WCollector::PostResolveCycleTask()'; do
    if ! /usr/bin/grep -F -q "$symbol" "$test_undef"; then
      echo "GC_UNIT_OHOS_HOST_PRODUCT_IMPORT_MISSING symbol=$symbol" >&2
      return 25
    fi
  done

  objdump -drC "$so" | sed -n \
    '/<MapleRuntime::WCollector::PostResolveCycleTask()>/,/^$/p' >"$post_disassembly"
  if ! /usr/bin/grep -F -q 'CJ_MRT_RolveCycleRef' "$post_disassembly"; then
    if [[ "${GC_UNIT_OHOS_HOST_ALLOW_MISSING_POST_DISPATCH:-0}" == "1" ]]; then
      echo "GC_UNIT_OHOS_HOST_POST_DISPATCH_MISSING_ALLOWED"
    else
      echo "GC_UNIT_OHOS_HOST_POST_DISPATCH_MISSING" >&2
      return 26
    fi
  fi

  sha256sum "$elf" "$so" "$bounds" >"$OUT/ohos_host_artifacts.sha256"
  {
    echo "BUILD_CAPTURED_AT=$(date --iso-8601=seconds)"
    echo "SOURCE_COMMIT=$(git -C "$ROOT" rev-parse HEAD)"
    echo "SOURCE_STATUS_BEGIN"
    git -C "$ROOT" status --porcelain
    echo "SOURCE_STATUS_END"
  } >"$OUT/ohos_host_lineage.txt"

  declare -a tests=(
    OHOSCycle.MajorEntryPostsResolveTask
    OHOSCycle.PostResolvePostsProductTask
    OHOSCycle.EmptyWorkDoesNotPost
  )
  declare -a keys=(MAJOR POST EMPTY)
  declare -a states=(NOT_RUN NOT_RUN NOT_RUN)
  declare -a rcs=(125 125 125)

  for i in "${!tests[@]}"; do
    test_name="${tests[$i]}"
    key="${keys[$i]}"
    set +e
    env LD_DEBUG=libs \
      LD_LIBRARY_PATH="$runroot:$RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
      "$elf" "--gtest_filter=$test_name" >"$OUT/ohos_host_${key,,}.log" 2>&1
    rc=$?
    set -e
    state=FAIL
    if [[ $rc -eq 0 ]] &&
        /usr/bin/grep -F -q "[  RUN   ] $test_name" "$OUT/ohos_host_${key,,}.log" &&
        /usr/bin/grep -F -q "[  PASS  ] $test_name" "$OUT/ohos_host_${key,,}.log" &&
        /usr/bin/grep -F -q "OHOS_HOST_ASSERT_REACHED test=$test_name" "$OUT/ohos_host_${key,,}.log"; then
      state=PASS
    else
      overall_rc=1
    fi
    states[$i]="$state"
    rcs[$i]="$rc"
    echo "$rc" >"$OUT/ohos_host_${key,,}.rc"
    echo "GC_UNIT_OHOS_HOST_FILTER test=$test_name state=$state rc=$rc log=$OUT/ohos_host_${key,,}.log"
  done

  {
    echo "SCHEMA_VERSION=1"
    echo "CONFIGURATION=MRT_GC_UNIT_OHOS_HOST"
    echo "PRODUCT_RECEIPT=MRT_GC_UNIT_OHOS_HOST_RECEIPT"
    for i in "${!tests[@]}"; do
      echo "FILTER_${keys[$i]}=${states[$i]}"
      echo "FILTER_${keys[$i]}_RC=${rcs[$i]}"
    done
    echo "RESULT=$([[ $overall_rc -eq 0 ]] && echo PASS || echo FAIL)"
    sha256sum "$elf" "$so" "$bounds"
  } >"$receipt"

  if [[ $overall_rc -ne 0 ]]; then
    echo "GC_UNIT_OHOS_HOST_FAIL receipt=$receipt" >&2
    return "$overall_rc"
  fi
  echo "GC_UNIT_OHOS_HOST_OK filters=3 receipt=$receipt elf=$elf"
}

case "${MRT_GC_UNIT_OHOS_HOST:-0}" in
  0) ;;
  1)
    run_ohos_host_arm
    exit $?
    ;;
  *)
    echo "error: MRT_GC_UNIT_OHOS_HOST must be 0 or 1" >&2
    exit 2
    ;;
esac

TEST_DEFINES=(-DMRT_ZSTAT_COMPILED=1)
RANGE_REGISTRY_FLAGS=()
RANGE_REGISTRY_SOURCES=()
if [[ "${MRT_TESTABLE_INTERNALS:-0}" == "1" ]]; then
  range_registry_symbols=$(nm -D "$RUNTIME_LIB_DIR/libcangjie-runtime.so" 2>/dev/null | \
    /usr/bin/grep -c 'RangeRegistry' || true)
  if [[ "$range_registry_symbols" -eq 0 ]]; then
    echo "error: MRT_TESTABLE_INTERNALS=1 but product SO has no RangeRegistry symbols" >&2
    exit 6
  fi
  RANGE_REGISTRY_FLAGS=(-DMRT_TESTABLE_INTERNALS=1)
  RANGE_REGISTRY_SOURCES=("$SRC/test_range_registry.cpp")
fi

# Keep the standalone test translation units in the same compile-time
# configuration as the product SO they bind. The default SO deliberately has
# neither test-only export; an MRT_GC_UNIT_TESTS SO must compile both integration
# suites into this executable so a partial product configuration fails at link.
nm -D "$RUNTIME_LIB_DIR/libcangjie-runtime.so" >"$OUT/runtime-dynamic-symbols.txt"
if /usr/bin/grep -Eq \
    'ShouldWaitForIgnoredGcRequest|CJ_MRT_SetLargeArrayInitTestHooks|SetAllocationStallTestHooks|PendingStalledAllocations' \
    "$OUT/runtime-dynamic-symbols.txt"; then
  TEST_DEFINES+=(-DMRT_GC_UNIT_TESTS=1)
  echo "GC_UNIT_PRODUCT_CONFIGURATION=MRT_GC_UNIT_TESTS"
else
  echo "GC_UNIT_PRODUCT_CONFIGURATION=DEFAULT"
fi
if /usr/bin/grep -Eq 'SetAllocationStallTestHooks|PendingStalledAllocations' \
    "$OUT/runtime-dynamic-symbols.txt"; then
  STALL_PRODUCT_OBSERVE=1
else
  STALL_PRODUCT_OBSERVE=0
fi
echo "STALL_PRODUCT_OBSERVE=$STALL_PRODUCT_OBSERVE"

# The M0 counter accessor is deliberately absent from the default product. Compile its five
# observer tests only when the linked SO was built with MRT_GC_UNIT_TESTS=ON.
M0_TEST_FLAGS=()
M0_TEST_SOURCES=()
M0_TEST_ACCESS=off
if nm -D --defined-only "$RUNTIME_LIB_DIR/libcangjie-runtime.so" 2>/dev/null | c++filt |
    /usr/bin/grep 'M0ExitDiagnostics::GetCounts' >/dev/null; then
  M0_TEST_FLAGS=(-DMRT_GC_UNIT_TEST_ACCESS=1)
  M0_TEST_SOURCES=("$SRC/test_m0_exit.cpp")
  M0_TEST_ACCESS=on
fi
echo "M0_TEST_ACCESS=$M0_TEST_ACCESS"

M0_CORRELATION_TEST_ARGS=()
M0_CORRELATION_ENV=()
if nm -D --defined-only "$RUNTIME_LIB_DIR/libcangjie-runtime.so" 2>/dev/null | c++filt |
    /usr/bin/grep 'M0Correlation::ResetForTest' >/dev/null; then
  TEST_DEFINES+=(-DMRT_M0_CORRELATION_EXPERIMENT=1 -DMRT_GC_UNIT_TEST_ACCESS=1)
  M0_CORRELATION_TEST_ARGS=("$SRC/test_m0_correlation.cpp")
  M0_CORRELATION_ENV=(MRT_GCV2_DIAG=m0corr)
fi

# Compile the publication TU with the same testability shape as the linked
# product SO. The default (OFF) SO has no retain hook, so it must not silently
# register a test that can only skip; the ON arm keeps the explicit precondition
# assertion in clear_entries_product_unit.cpp.
PUBLICATION_TESTABLE_FLAGS=()
if nm -D --defined-only "$RUNTIME_LIB_DIR/libcangjie-runtime.so" 2>/dev/null |
    c++filt | /usr/bin/grep 'ForwardingTable::SetLookupRetainHook' >/dev/null; then
  PUBLICATION_TESTABLE_FLAGS=(-DMRT_FINDTO_RETAIN_TEST=1)
fi
echo "PUBLICATION_TESTABLE=$((${#PUBLICATION_TESTABLE_FLAGS[@]} != 0))"

# These three deterministic publication tests require both ends of their
# scheduling fixture.  Derive that product shape from the linked SO, not from
# the test translation unit's unconditional MRT_TESTABLE_INTERNALS definition.
PUBLICATION_HOOK_TESTS=(
  ForwardingPublicationProduct.MutatorRuntimeEntryReachesCopyAdmission
  ForwardingPublicationProduct.CopyAdmissionSealWaitsRealCopierAndRejectsLateEntry
  ForwardingPublicationProduct.AdmittedCopierExitsWhilePeerEntering
)
PUBLICATION_HOOK_FLAGS=()
PUBLICATION_HOOK_EXPORTS="$OUT/forwarding-publication-hook-exports.txt"
nm -D --defined-only "$RUNTIME_LIB_DIR/libcangjie-runtime.so" | c++filt >"$PUBLICATION_HOOK_EXPORTS"
admission_hook=0
receipt_hook=0
if /usr/bin/grep -F -q 'MRT_SetCopyAdmissionTestHook' "$PUBLICATION_HOOK_EXPORTS"; then
  admission_hook=1
fi
if /usr/bin/grep -F -q 'MapleRuntime::ForwardingTable::SetReceiptLifeRegisterHook(' \
    "$PUBLICATION_HOOK_EXPORTS"; then
  receipt_hook=1
fi
if [[ "$admission_hook" -eq 1 && "$receipt_hook" -eq 1 ]]; then
  PUBLICATION_HOOK_PRODUCT_SHAPE=testable
  PUBLICATION_HOOK_FLAGS=(-DMRT_FORWARDING_PUBLICATION_HOOKS_AVAILABLE=1)
elif [[ "$admission_hook" -eq 0 && "$receipt_hook" -eq 0 ]]; then
  PUBLICATION_HOOK_PRODUCT_SHAPE=default
else
  echo "GC_UNIT_PUBLICATION_HOOK_PRODUCT_SHAPE_INCOMPLETE admission=$admission_hook receipt=$receipt_hook" >&2
  exit 16
fi
echo "PUBLICATION_HOOK_PRODUCT_SHAPE=$PUBLICATION_HOOK_PRODUCT_SHAPE admission=$admission_hook receipt=$receipt_hook"

# The publication TU always has fixture access, but the linked product SO only
# owns the actual-entry receipt in a testable product build. Derive that shape
# from both receipt endpoints; a partial export is an invalid product shape.
REMAP_RECEIPT_TEST=LoadHealDeliveryProduct.MajorDispatchRemapsLiveRemoteArrayField
REMAP_RECEIPT_FLAGS=()
REMAP_RECEIPT_EXPORTS="$OUT/remap-young-roots-receipt-exports.txt"
nm -D --defined-only "$RUNTIME_LIB_DIR/libcangjie-runtime.so" | c++filt >"$REMAP_RECEIPT_EXPORTS"
remap_receipt_reset=0
remap_receipt_read=0
if /usr/bin/grep -F -q 'MapleRuntime::ResetRemapYoungRootsTestReceipt(' "$REMAP_RECEIPT_EXPORTS"; then
  remap_receipt_reset=1
fi
if /usr/bin/grep -F -q 'MapleRuntime::ReadRemapYoungRootsTestReceipt(' "$REMAP_RECEIPT_EXPORTS"; then
  remap_receipt_read=1
fi
if [[ "$remap_receipt_reset" -eq 1 && "$remap_receipt_read" -eq 1 ]]; then
  REMAP_RECEIPT_PRODUCT_SHAPE=testable
  REMAP_RECEIPT_FLAGS=(-DMRT_REMAP_YOUNG_ROOTS_RECEIPT_AVAILABLE=1)
elif [[ "$remap_receipt_reset" -eq 0 && "$remap_receipt_read" -eq 0 ]]; then
  REMAP_RECEIPT_PRODUCT_SHAPE=default
else
  echo "GC_UNIT_REMAP_RECEIPT_PRODUCT_SHAPE_INCOMPLETE reset=$remap_receipt_reset read=$remap_receipt_read" >&2
  exit 19
fi
echo "REMAP_RECEIPT_PRODUCT_SHAPE=$REMAP_RECEIPT_PRODUCT_SHAPE reset=$remap_receipt_reset read=$remap_receipt_read"

BOUNDS_INC="$ROOT/runtime/third_party/third_party_bounds_checking_function/include"
TESTABLE_FLAGS=()
if [[ "${MRT_TESTABLE_INTERNALS:-0}" == "1" ]]; then
  TESTABLE_FLAGS+=(-DMRT_TESTABLE_INTERNALS=1)
fi
INC_FLAGS=(
  -I"$SRC"
  -I"$ROOT/runtime/src"
  -I"$ROOT/runtime/src/Heap"
  -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include"
  -I"$ROOT/runtime/include"
  -I"$BOUNDS_INC"
)
RUNTIME_OUTPUT_ROOT="${GCV2_RUNTIME_OUTPUT_ROOT:-$(realpath -m "$RUNTIME_LIB_DIR/../..")}"
if [[ -d "$RUNTIME_OUTPUT_ROOT/include" ]]; then
  INC_FLAGS+=(-I"$RUNTIME_OUTPUT_ROOT/include")
fi

validate_mutualwait_manifest
if [[ "${GC_UNIT_MUTUALWAIT_MANIFEST_ONLY:-0}" == "1" ]]; then
  exit 0
fi

# A weak referent is a discovery input, not a strong tracing root. Keep this
# source-level consumer guard next to the product-linked behavior tests: the
# positive anchor proves the guard inspected the active collector source, and
# reintroducing the old referent traversal fails before any test can pass.
WEAK_DISCOVERY_SOURCE="$ROOT/runtime/src/Heap/Collector/TracingCollector.cpp"
if ! /usr/bin/grep -F -q \
    'collector.DiscoverWeakReference(obj, workStack)' "$WEAK_DISCOVERY_SOURCE" ||
    ! /usr/bin/grep -F -q \
    'DiscoverReference(reference, ReferenceType::WEAK)' "$WEAK_DISCOVERY_SOURCE"; then
  echo "GC_UNIT_WEAK_DISCOVERY_ANCHOR_MISSING" >&2
  exit 11
fi
if /usr/bin/grep -F -q \
    'TraceObjectRefFields(referent, workStack)' "$WEAK_DISCOVERY_SOURCE"; then
  echo "GC_UNIT_WEAK_REFERENT_TRACED_STRONGLY" >&2
  exit 12
fi
echo "GATE_WEAK_DISCOVERY_NO_STRONG_TRACE_OK source=$WEAK_DISCOVERY_SOURCE"

# Keep this hand-driven entry point structurally identical to the CMake
# cj_gc_unit target: product inline/template helpers stay hidden and static
# archives cannot re-export weak copies of the product symbols exercised via
# dlsym in test_live_map.cpp. Compile each translation unit independently so
# kkk2 can use its cores; link in the original source order.
MAIN_COMPILE_FLAGS=(
  -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti
  -fvisibility-inlines-hidden
  "${RANGE_REGISTRY_FLAGS[@]}"
  "${TEST_DEFINES[@]}"
  "${TESTABLE_FLAGS[@]}"
  "${M0_TEST_FLAGS[@]}"
  "${INC_FLAGS[@]}"
)
MAIN_SOURCES=(
  "$SRC/gc_unit_main.cpp"
  "$SRC/gc_unit_stubs.cpp"
  "$ROOT/runtime/src/Base/ZStat.cpp"
  "$SRC/test_colour_address.cpp"
  "$SRC/test_z_bit_field.cpp"
  "$SRC/test_z_list.cpp"
  "$SRC/test_zstat.cpp"
  "$SRC/test_trustp1_phase1.cpp"
  "$SRC/test_route_info.cpp"
  "$SRC/test_live_map.cpp"
  "$SRC/test_object_gate.cpp"
  "$SRC/test_remset.cpp"
  "$SRC/test_defect_regressions.cpp"
  "$SRC/test_region_bitmap.cpp"
  "$SRC/test_region_age.cpp"
  "$SRC/test_unwind_regressions.cpp"
  "$SRC/test_gctibzero.cpp"
  "$SRC/test_pinroot.cpp"
  "$SRC/test_followedge.cpp"
  "$SRC/test_z_forwarding_life.cpp"
  "$SRC/test_colour_is_checks.cpp"
  "$SRC/test_remap_young_roots.cpp"
  "$SRC/test_forwarding_entries.cpp"
  "$SRC/test_forwarding_no_geometry.cpp"
  "$SRC/test_z_forwarding_table.cpp"
  "$SRC/test_allocation_stall_queue.cpp"
  "$SRC/test_young_conc.cpp"
  "$SRC/test_alloc_buffer_handoff.cpp"
  "$SRC/test_young_weak.cpp"
  "$SRC/test_relocation_set_selector.cpp"
  "$SRC/test_store_barrier_buffer.cpp"
  "$SRC/test_barrier_old_atomic.cpp"
  "$SRC/test_page_age.cpp"
  "${RANGE_REGISTRY_SOURCES[@]}"
  "$SRC/test_stay_young.cpp"
  "$SRC/test_gc_trigger.cpp"
  "$SRC/test_gc_request_sync.cpp"
  "$SRC/test_mutator_relocate.cpp"
  "$SRC/test_uncommitter.cpp"
  "$SRC/test_relocation_request_queue.cpp"
  "$SRC/test_gc_thread_pool.cpp"
  "$SRC/test_expire_kept.cpp"
  "$SRC/test_receipt_life.cpp"
  "$SRC/test_receipt_life_registry.cpp"
  "$SRC/test_lifeclock.cpp"
  "$SRC/test_exempt_unlock.cpp"
  "$SRC/test_heal_coverage.cpp"
  "$SRC/test_diag_gate.cpp"
  "$SRC/test_interior_edge_class.cpp"
  "$SRC/test_isfromreg.cpp"
  "$SRC/test_current_object_ref.cpp"
  "$SRC/test_fillerobj.cpp"
  "$SRC/test_i2_readref.cpp"
  "${M0_TEST_SOURCES[@]}"
  "$SRC/test_loadfc.cpp"
  "${M0_CORRELATION_TEST_ARGS[@]}"
  "$SRC/test_fwdreturn.cpp"
  "$SRC/test_ghost_region_lookup.cpp"
  "$SRC/test_fnlz_roots.cpp"
  "$SRC/test_reference_processor.cpp"
  "$SRC/test_mark_stack_entry.cpp"
  "$SRC/test_mark_stripe.cpp"
  "$SRC/test_mark_port_203_storage.cpp"
  "$SRC/test_mark_port_203_entries.cpp"
  "$SRC/test_mark_port_203_engine.cpp"
  "$SRC/test_partial_array.cpp"
  "$SRC/test_segmented_array_init.cpp"
  "$SRC/test_verify_roots.cpp"
  "$SRC/test_verify_fail_close.cpp"
  "$SRC/test_verify_phase.cpp"
  "$SRC/test_verify_marking_stacks.cpp"
  "$SRC/test_mem_map.cpp"
  "$SRC/test_colour_census.cpp"
  "$SRC/test_payload_clamp.cpp"
  "$SRC/test_cycle_ref_saferegion.cpp"
)

PUBLICATION_COMPILE_FLAGS=(
  -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti
  -fvisibility-inlines-hidden
  "${TEST_DEFINES[@]}"
  -DMRT_TESTABLE_INTERNALS=1
  "${PUBLICATION_TESTABLE_FLAGS[@]}"
  "${PUBLICATION_HOOK_FLAGS[@]}"
  "${REMAP_RECEIPT_FLAGS[@]}"
  "${INC_FLAGS[@]}"
)
PUBLICATION_SOURCES=(
  "$SRC/gc_unit_main.cpp"
  "$SRC/clear_entries_product_unit.cpp"
)

BUILD_JOBS="${GC_UNIT_BUILD_JOBS:-$(nproc)}"
if [[ ! "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "error: GC_UNIT_BUILD_JOBS must be a positive integer, got: $BUILD_JOBS" >&2
  exit 2
fi
MAIN_OBJECT_DIR="$OUT/objects/main"
PUBLICATION_OBJECT_DIR="$OUT/objects/publication"
mkdir -p "$MAIN_OBJECT_DIR" "$PUBLICATION_OBJECT_DIR"
MAIN_OBJECTS=()
PUBLICATION_OBJECTS=()
COMPILE_MANIFEST="$OUT/compile-manifest.bin"
: >"$COMPILE_MANIFEST"
for index in "${!MAIN_SOURCES[@]}"; do
  object="$MAIN_OBJECT_DIR/$(printf '%04d' "$index")-$(basename "${MAIN_SOURCES[$index]}").o"
  MAIN_OBJECTS+=("$object")
  printf 'main\0%s\0%s\0' "$object" "${MAIN_SOURCES[$index]}" >>"$COMPILE_MANIFEST"
done
for index in "${!PUBLICATION_SOURCES[@]}"; do
  object="$PUBLICATION_OBJECT_DIR/$(printf '%04d' "$index")-$(basename "${PUBLICATION_SOURCES[$index]}").o"
  PUBLICATION_OBJECTS+=("$object")
  printf 'publication\0%s\0%s\0' "$object" "${PUBLICATION_SOURCES[$index]}" >>"$COMPILE_MANIFEST"
done

printf -v MAIN_COMPILE_FLAGS_SERIALIZED '%s\n' "${MAIN_COMPILE_FLAGS[@]}"
MAIN_COMPILE_FLAGS_SERIALIZED=${MAIN_COMPILE_FLAGS_SERIALIZED%$'\n'}
printf -v PUBLICATION_COMPILE_FLAGS_SERIALIZED '%s\n' "${PUBLICATION_COMPILE_FLAGS[@]}"
PUBLICATION_COMPILE_FLAGS_SERIALIZED=${PUBLICATION_COMPILE_FLAGS_SERIALIZED%$'\n'}
export CXX MAIN_COMPILE_FLAGS_SERIALIZED PUBLICATION_COMPILE_FLAGS_SERIALIZED
compile_one() {
  local target=$1 object=$2 source=$3 serialized
  local -a flags compiler
  case "$target" in
    main) serialized=$MAIN_COMPILE_FLAGS_SERIALIZED ;;
    publication) serialized=$PUBLICATION_COMPILE_FLAGS_SERIALIZED ;;
    *) return 2 ;;
  esac
  mapfile -t flags <<<"$serialized"
  read -r -a compiler <<<"$CXX"
  "${compiler[@]}" "${flags[@]}" -c "$source" -o "$object"
}
export -f compile_one
xargs -0 -n 3 -P "$BUILD_JOBS" bash -c 'compile_one "$1" "$2" "$3"' _ <"$COMPILE_MANIFEST"

read -r -a CXX_COMMAND <<<"$CXX"
set +e
(
  "${CXX_COMMAND[@]}" "${MAIN_COMPILE_FLAGS[@]}" "${MAIN_OBJECTS[@]}" \
    -L"$RUNTIME_LIB_DIR" -Wl,-rpath,"$RUNTIME_LIB_DIR" -Wl,--exclude-libs,ALL \
    -lcangjie-runtime -lboundscheck -o "$OUT/cj_gc_unit"
) &
main_link_pid=$!
(
  "${CXX_COMMAND[@]}" "${PUBLICATION_COMPILE_FLAGS[@]}" "${PUBLICATION_OBJECTS[@]}" \
    -L"$RUNTIME_LIB_DIR" -Wl,-rpath,"$RUNTIME_LIB_DIR" -Wl,--exclude-libs,ALL \
    -lcangjie-runtime -lboundscheck -o "$OUT/cj_gc_forwarding_publication_unit"
) &
publication_link_pid=$!
wait "$main_link_pid"
main_link_rc=$?
wait "$publication_link_pid"
publication_link_rc=$?
set -e
if [[ $main_link_rc -ne 0 || $publication_link_rc -ne 0 ]]; then
  echo "GC_UNIT_LINK_FAIL main_rc=$main_link_rc publication_rc=$publication_link_rc" >&2
  exit 2
fi
echo "GC_UNIT_COMPILE_PARALLEL jobs=$BUILD_JOBS tus=$((${#MAIN_SOURCES[@]} + ${#PUBLICATION_SOURCES[@]}))"

# The standalone script is the frozen gate's real build entry point.  Keep the
# same structural invariant as the CMake target at that point, before any test
# process can run: none of the product consumers exercised through dlsym may
# be dynamically defined by this executable itself.  A removed visibility or
# archive-exclusion flag therefore fails closed instead of silently restoring
# the old self-satisfying weak copies.
STANDALONE_SYMBOLS=(
  _ZN12MapleRuntime10RegionInfo10MarkObjectILNS_10GenerationE0EEEbNS_8MarkViewIXT_EEEPKNS_10BaseObjectEmb
  _ZN12MapleRuntime10RegionInfo10MarkObjectILNS_10GenerationE1EEEbNS_8MarkViewIXT_EEEPKNS_10BaseObjectEmb
  _ZN12MapleRuntime10RegionInfo13ClearLiveInfoILNS_10GenerationE0EEEvNS_8MarkViewIXT_EEE
  _ZN12MapleRuntime10RegionInfo24PreserveRetainedLiveInfoEv
  _ZN12MapleRuntime10RegionInfo31BumpSnapshotEpochFromInitRegionEv
  _ZNK12MapleRuntime10WCollector10MarkObjectEPNS_10BaseObjectE
  _ZN12MapleRuntime10SatbBuffer13ShouldEnqueueEPKNS_10BaseObjectE
)
STANDALONE_FULL_SYMBOLS=(
  _ZN12MapleRuntime10RegionInfo28PreserveRetainedLiveInfoUpToEm
  CJ_MCC_PostWriteRefField
)
# RegionInfo::MarkObject templates are instantiated by other TUs in this ELF.
# The concurrent item binds the 4-arg Old instantiation via dlsym only; Tcut×P0
# is the structural proof that item does not use the local copy.
STANDALONE_SYMBOL_DYN="$OUT/cj_gc_unit.dynamic-defined.txt"
STANDALONE_SYMBOL_FULL="$OUT/cj_gc_unit.full-defined.txt"
nm -D --defined-only "$OUT/cj_gc_unit" >"$STANDALONE_SYMBOL_DYN"
nm --defined-only "$OUT/cj_gc_unit" >"$STANDALONE_SYMBOL_FULL"
if ! /usr/bin/grep -Eq '[[:space:]]main$' "$STANDALONE_SYMBOL_FULL"; then
  echo "GC_UNIT_STANDALONE_SYMBOL_GUARD_BROKEN positive_control=main" >&2
  exit 7
fi
for symbol in "${STANDALONE_SYMBOLS[@]}"; do
  if /usr/bin/grep -F -q "$symbol" "$STANDALONE_SYMBOL_DYN"; then
    echo "GC_UNIT_STANDALONE_SYMBOL_GUARD_FAIL symbol=$symbol" >&2
    exit 7
  fi
done
for symbol in "${STANDALONE_FULL_SYMBOLS[@]}"; do
  if /usr/bin/grep -F -q "$symbol" "$STANDALONE_SYMBOL_DYN" ||
      /usr/bin/grep -F -q "$symbol" "$STANDALONE_SYMBOL_FULL"; then
    echo "GC_UNIT_STANDALONE_SYMBOL_GUARD_FAIL symbol=$symbol" >&2
    exit 7
  fi
done
echo "GATE_STANDALONE_SYMBOLS_OK elf=$OUT/cj_gc_unit"

# Receipt-life product binding: nm of the test ELF vs product SO.
# Wiring evidence is the fault-arm patches + seven-cut behavioral tests.
# No static manifest lint — see rev_mw_r7 net-value judgment.

RECEIPT_LIFE_FULL="$OUT/cj_gc_unit.full-defined-receipt-life.txt"
RECEIPT_LIFE_UNDEFINED="$OUT/cj_gc_unit.undefined-receipt-life.txt"
RECEIPT_LIFE_EXPORTS="$OUT/runtime.exports-receipt-life.txt"
nm --defined-only "$OUT/cj_gc_unit" | c++filt >"$RECEIPT_LIFE_FULL"
nm -u "$OUT/cj_gc_unit" | c++filt >"$RECEIPT_LIFE_UNDEFINED"
nm -D --defined-only "$RUNTIME_LIB_DIR/libcangjie-runtime.so" | c++filt >"$RECEIPT_LIFE_EXPORTS"
for symbol in 'MapleRuntime::ForwardingTable::InstallMapping(' \
              'MapleRuntime::ForwardingTable::FindTo('; do
  if /usr/bin/grep -F -q "$symbol" "$RECEIPT_LIFE_FULL"; then
    echo "GC_UNIT_RECEIPT_LIFE_BINDING_FAIL local_definition=$symbol" >&2
    exit 7
  fi
  /usr/bin/grep -F -q "$symbol" "$RECEIPT_LIFE_UNDEFINED"
  /usr/bin/grep -F -q "$symbol" "$RECEIPT_LIFE_EXPORTS"
done
if ! /usr/bin/grep -F -q 'ReceiptLifeRegistry_' "$RECEIPT_LIFE_FULL"; then
  echo "GC_UNIT_RECEIPT_LIFE_BINDING_FAIL positive_control=ReceiptLifeRegistry" >&2
  exit 7
fi
echo "GATE_RECEIPT_LIFE_PRODUCT_BINDING_OK elf=$OUT/cj_gc_unit"

# The compiler old-value test is a product-path test with an independently
# replaceable carrier.  Keep its target set outside the test call itself so
# deleting the consumer cannot silently shrink this guard.  Full nm excludes
# local/weak copies; the undefined import and product export bind the call to
# the runtime SO used by the 2x2 cut/restore matrix.
OLDVALUE_MANIFEST="$SRC/product_call_manifest_oldvalue.tsv"
EXPECTED_OLDVALUE_TESTS=(
  StoreBuf.CompilerFastOverwriteHandsObservedOldToSatb
)
oldvalue_rows=0
while IFS=$'\t' read -r test_name anchor carrier consumer cut_site; do
  if [[ "$test_name" == "test_name" ]]; then
    continue
  fi
  [[ "$anchor" == "CJ_MCC_PostWriteRefField" ]]
  [[ "$carrier" == "product_so" ]]
  [[ "$consumer" == "CJ_MCC_PostWriteRefField(newReferent, holder, &field, observedPrev)" ]]
  suite="${test_name%%.*}"
  name="${test_name#*.}"
  /usr/bin/grep -F -q "GC_TEST($suite, $name)" "$SRC/test_store_barrier_buffer.cpp"
  /usr/bin/grep -F -q "$anchor" "$ROOT/runtime/src/CompilerCalls.cpp"
  /usr/bin/grep -F -q "$consumer" "$SRC/test_store_barrier_buffer.cpp"
  /usr/bin/grep -F -q "$cut_site" "$ROOT/runtime/src/Heap/Barrier/Barrier.cpp"
  oldvalue_rows=$((oldvalue_rows + 1))
done <"$OLDVALUE_MANIFEST"
[[ "$oldvalue_rows" -eq "${#EXPECTED_OLDVALUE_TESTS[@]}" ]]
for test_name in "${EXPECTED_OLDVALUE_TESTS[@]}"; do
  /usr/bin/grep -q "^${test_name}"$'\t' "$OLDVALUE_MANIFEST"
done
[[ $(/usr/bin/grep -F -c 'CJ_MCC_PostWriteRefField(newReferent, holder, &field, observedPrev)' \
  "$SRC/test_store_barrier_buffer.cpp") -eq "$oldvalue_rows" ]]
OLDVALUE_UNDEFINED="$OUT/cj_gc_unit.undefined-oldvalue.txt"
nm -u "$OUT/cj_gc_unit" >"$OLDVALUE_UNDEFINED"
if ! /usr/bin/grep -F -q 'CJ_MCC_PostWriteRefField' "$OLDVALUE_UNDEFINED"; then
  echo "GC_UNIT_OLDVALUE_IMPORT_MISSING symbol=CJ_MCC_PostWriteRefField" >&2
  exit 10
fi
if ! /usr/bin/grep -F -q 'CJ_MCC_PostWriteRefField' "$OUT/runtime-dynamic-symbols.txt"; then
  echo "GC_UNIT_OLDVALUE_EXPORT_MISSING symbol=CJ_MCC_PostWriteRefField" >&2
  exit 10
fi
echo "GATE_OLDVALUE_PRODUCT_BINDING_OK rows=$oldvalue_rows elf=$OUT/cj_gc_unit"
STALL_TEST_DEFINED=$(nm --defined-only "$OUT/cj_gc_unit" | /usr/bin/grep -c 'AllocationStall_' || true)
echo "STALL_TEST_DEFINED=$STALL_TEST_DEFINED"
if [[ "$STALL_PRODUCT_OBSERVE" -eq 1 && "$STALL_TEST_DEFINED" -eq 0 ]]; then
  echo "GC_UNIT_GATE_FAIL: product SO exports stall observers but the test ELF registered no AllocationStall tests" >&2
  exit 8
fi
if [[ "$STALL_PRODUCT_OBSERVE" -eq 0 && "$STALL_TEST_DEFINED" -ne 0 ]]; then
  echo "GC_UNIT_GATE_FAIL: stall tests compiled against a product SO with no stall observers" >&2
  exit 8
fi
if [[ "$STALL_PRODUCT_OBSERVE" -eq 0 ]]; then
  echo "STALL_SUITE=SKIP_DEFAULT_SO"
fi

# The target set is independent of the dlsym calls currently left in the test
# source.  Deleting a test/call or shrinking the manifest therefore fails
# closed instead of silently reducing the binding guard's coverage.
PRODUCT_PATH_MANIFEST="$SRC/product_path_manifest.tsv"
EXPECTED_BOUNDED_TESTS=(
  LiveMap.UnexaminedRelocselPageKeepsWithoutSnapshot
  LiveMap.ExaminedPageWithoutSnapshotStillAborts
  LiveMap.OwnedCopyExaminedPageWithoutSnapshotStillAborts
)
manifest_rows=0
while IFS=$'\t' read -r test_name anchor carrier consumer cut_site; do
  if [[ "$test_name" == "test_name" ]]; then
    continue
  fi
  if [[ "$test_name" == Uncommitter.* ]]; then
    echo "SKIP_PRODUCT_PATH_UNCOMMIT test_name=$test_name anchor=$anchor carrier=$carrier consumer=$consumer cut_site=$cut_site"
    continue
  fi
  [[ "$anchor" == "_ZN12MapleRuntime10RegionInfo28PreserveRetainedLiveInfoUpToEm" ]]
  [[ "$carrier" == "product_so" ]]
  [[ "$consumer" == "ProductPreserveRetainedUpToFn" ]]
  /usr/bin/grep -F -q "$cut_site" "$ROOT/runtime/src/Heap/Allocator/RegionInfo.h"
  suite="${test_name%%.*}"
  name="${test_name#*.}"
  /usr/bin/grep -F -q "GC_TEST($suite, $name)" "$SRC/test_live_map.cpp"
  manifest_rows=$((manifest_rows + 1))
done <"$PRODUCT_PATH_MANIFEST"
[[ "$manifest_rows" -eq "${#EXPECTED_BOUNDED_TESTS[@]}" ]]
for test_name in "${EXPECTED_BOUNDED_TESTS[@]}"; do
  /usr/bin/grep -F -q "$test_name" "$PRODUCT_PATH_MANIFEST"
done
actual_bounded_calls=$(/usr/bin/grep -F -c 'ProductPreserveRetainedUpToFn()(' "$SRC/test_live_map.cpp")
[[ "$actual_bounded_calls" -eq "$manifest_rows" ]]
echo "GATE_PRODUCT_PATH_MANIFEST_OK rows=$manifest_rows bounded_calls=$actual_bounded_calls"

# ReferenceProcessor is an independently replaceable product carrier. Guard
# full symbols (not only the dynamic table) so no local/weak test copy can
# satisfy its consumers, then require the executable to import those methods.
REFERENCE_PROCESSOR_CONSUMERS=(
  'MapleRuntime::ReferenceProcessor::DiscoverReference('
  'MapleRuntime::ReferenceProcessor::ProcessReferences('
  'MapleRuntime::ReferenceProcessor::EnqueueReferences('
  'MapleRuntime::TracingCollector::DiscoverWeakReference('
)
REFERENCE_PROCESSOR_FULL="$OUT/cj_gc_unit.full-defined.txt"
REFERENCE_PROCESSOR_UNDEFINED="$OUT/cj_gc_unit.undefined.txt"
nm --defined-only "$OUT/cj_gc_unit" | c++filt >"$REFERENCE_PROCESSOR_FULL"
nm -u "$OUT/cj_gc_unit" | c++filt >"$REFERENCE_PROCESSOR_UNDEFINED"
if ! /usr/bin/grep -Eq '[[:space:]]main$' "$REFERENCE_PROCESSOR_FULL"; then
  echo "GC_UNIT_FULL_NM_POSITIVE_CONTROL_FAIL symbol=main" >&2
  exit 8
fi
for consumer in "${REFERENCE_PROCESSOR_CONSUMERS[@]}"; do
  if /usr/bin/grep -F -q "$consumer" "$REFERENCE_PROCESSOR_FULL"; then
    echo "GC_UNIT_REFERENCE_PROCESSOR_LOCAL_DEFINITION symbol=$consumer" >&2
    exit 9
  fi
  if ! /usr/bin/grep -F -q "$consumer" "$REFERENCE_PROCESSOR_UNDEFINED"; then
    echo "GC_UNIT_REFERENCE_PROCESSOR_IMPORT_MISSING symbol=$consumer" >&2
    exit 10
  fi
done
echo "GATE_REFERENCE_PROCESSOR_BINDING_OK elf=$OUT/cj_gc_unit"

if [[ "${MRT_TESTABLE_INTERNALS:-0}" == "1" ]]; then
  YOUNG_WEAK_PRODUCT_CONSUMERS=(
    'MapleRuntime::WCollector::DoGarbageCollection()'
    'MapleRuntime::WCollector::TraceHeap()'
    'MapleRuntime::ResetYoungWeakClosureTestReceipt()'
    'MapleRuntime::ReadYoungWeakClosureTestReceipt()'
    'MapleRuntime::ResetWeakDiscoveryTestReceipt()'
    'MapleRuntime::ReadWeakDiscoveryTestReceipt()'
  )
  for consumer in "${YOUNG_WEAK_PRODUCT_CONSUMERS[@]}"; do
    if /usr/bin/grep -F -q "$consumer" "$REFERENCE_PROCESSOR_FULL"; then
      echo "GC_UNIT_YOUNG_WEAK_LOCAL_DEFINITION symbol=$consumer" >&2
      exit 14
    fi
    if ! /usr/bin/grep -F -q "$consumer" "$REFERENCE_PROCESSOR_UNDEFINED"; then
      echo "GC_UNIT_YOUNG_WEAK_IMPORT_MISSING symbol=$consumer" >&2
      exit 15
    fi
  done
  echo "GATE_YOUNG_WEAK_PRODUCT_BINDING_OK elf=$OUT/cj_gc_unit"
fi

# Load-heal delivery tests bind four independently replaceable product
# consumers.  The manifest is independent of the calls currently present in
# the test source, so deleting a test or anchor shrinks neither guard silently.
LOADHEAL_PRODUCT_CONSUMERS=(
  'MapleRuntime::ForwardingTable::PublishFromPageView('
  'MapleRuntime::ForwardingTable::GetFromPageView('
  'MapleRuntime::PromotedRegionDomain::DischargeAll('
  'MapleRuntime::RememberedSet::MoveInPlaceSlots('
  'MapleRuntime::RegionManager::RecordPinnedCrossGenEdges('
  'MapleRuntime::WCollector::RemapYoungRoots('
  'MapleRuntime::RegionManager::FinishIncompleteFromRegions('
  'MapleRuntime::ForwardingTable::ReclaimRetired('
)
if [[ "$REMAP_RECEIPT_PRODUCT_SHAPE" == testable ]]; then
  LOADHEAL_PRODUCT_CONSUMERS+=(
    'MapleRuntime::CopyCollector::RunGarbageCollection('
    'MapleRuntime::ResetRemapYoungRootsTestReceipt('
    'MapleRuntime::ReadRemapYoungRootsTestReceipt()'
  )
fi
LOADHEAL_MANIFEST="$SRC/product_call_manifest_loadheal.tsv"
EXPECTED_LOADHEAL_TESTS=(
  LoadHealDeliveryProduct.DualCarrierProducerCapturesOldTopAndLivemap
  LoadHealDeliveryProduct.DualCarrierConsumerSurvivesCurrentPageResetUntilRetire
  LoadHealDeliveryProduct.PromotedSnapshotDischargesOnlyLiveHolder
  LoadHealDeliveryProduct.InPlaceRemsetMovesBitAndFeedsConsumer
  LoadHealDeliveryProduct.CrossGenRangeGateRecordsLegalAndRejectsBeyondTop
  LoadHealDeliveryProduct.CurrentRemsetRemapsLiveRemoteArrayField
  LoadHealDeliveryProduct.MajorDispatchRemapsLiveRemoteArrayField
)
loadheal_rows=0
while IFS=$'\t' read -r test_name anchor carrier consumer cut_site; do
  if [[ "$test_name" == "test_name" ]]; then
    continue
  fi
  [[ "$carrier" == "product_so" ]]
  suite="${test_name%%.*}"
  name="${test_name#*.}"
  if ! /usr/bin/grep -F -q "GC_TEST($suite, $name)" "$SRC/clear_entries_product_unit.cpp" &&
     ! /usr/bin/grep -F -q "GC_OTHER_VM_TEST($suite, $name)" "$SRC/clear_entries_product_unit.cpp"; then
    echo "GC_UNIT_LOADHEAL_TEST_REGISTRATION_MISSING test=$test_name" >&2
    exit 10
  fi
  /usr/bin/grep -F -q "$consumer" "$SRC/clear_entries_product_unit.cpp"
  /usr/bin/grep -R -F -q "${anchor##*::}" "$ROOT/runtime/src/Heap"
  /usr/bin/grep -R -F -q "$cut_site" "$ROOT/runtime/src/Heap"
  loadheal_rows=$((loadheal_rows + 1))
done <"$LOADHEAL_MANIFEST"
[[ "$loadheal_rows" -eq "${#EXPECTED_LOADHEAL_TESTS[@]}" ]]
for test_name in "${EXPECTED_LOADHEAL_TESTS[@]}"; do
  /usr/bin/grep -q "^${test_name}"$'\t' "$LOADHEAL_MANIFEST"
done
echo "GATE_LOADHEAL_PRODUCT_MANIFEST_OK rows=$loadheal_rows source=clear_entries_product_unit.cpp"

# Pointer-colour census tests consume independently replaceable functions from
# the product SO.  Full nm excludes even local/weak test copies; nm -u proves
# the calls are imports.  main is the positive control above.
PTRCOLOUR_PRODUCT_CONSUMERS=()
PTRCOLOUR_PRODUCT_CONSUMERS+=('MapleRuntime::EnumBarrier::ReadReference(')
if [[ "${MRT_TESTABLE_INTERNALS:-0}" == "1" ]]; then
  PTRCOLOUR_PRODUCT_CONSUMERS+=('MapleRuntime::CensusObjectSlots(')
  PTRCOLOUR_PRODUCT_CONSUMERS+=('MapleRuntime::EnforceColourCensusForTesting(')
  PTRCOLOUR_PRODUCT_CONSUMERS+=('MapleRuntime::AssertColouredWriteIfEnabled(')
fi
for consumer in "${PTRCOLOUR_PRODUCT_CONSUMERS[@]}"; do
  if /usr/bin/grep -F -q "$consumer" "$REFERENCE_PROCESSOR_FULL"; then
    echo "GC_UNIT_PTRCOLOUR_LOCAL_DEFINITION symbol=$consumer" >&2
    exit 9
  fi
  if ! /usr/bin/grep -F -q "$consumer" "$REFERENCE_PROCESSOR_UNDEFINED"; then
    echo "GC_UNIT_PTRCOLOUR_IMPORT_MISSING symbol=$consumer" >&2
    exit 10
  fi
done

PTRCOLOUR_MANIFEST="$SRC/product_call_manifest_ptrcolour.tsv"
ptrcolour_rows=0
while IFS=$'\t' read -r test_name anchor carrier consumer cut_site; do
  if [[ "$test_name" == "test_name" ]]; then
    continue
  fi
  [[ "$carrier" == "product_so" ]]
  suite="${test_name%%.*}"
  name="${test_name#*.}"
  case "$test_name" in
    ColourCensus.*) test_source="$SRC/test_colour_census.cpp" ;;
    I2ReadRef.*) test_source="$SRC/test_i2_readref.cpp" ;;
    *) echo "GC_UNIT_PTRCOLOUR_UNKNOWN_TEST test_name=$test_name" >&2; exit 10 ;;
  esac
  /usr/bin/grep -F -q "GC_TEST($suite, $name)" "$test_source"
  /usr/bin/grep -R -F -q "${anchor##*::}" "$ROOT/runtime/src"
  /usr/bin/grep -F -q "${consumer#*:}" "$test_source"
  /usr/bin/grep -R -F -q "${cut_site#*:}" "$ROOT/runtime/src"
  ptrcolour_rows=$((ptrcolour_rows + 1))
done <"$PTRCOLOUR_MANIFEST"
[[ "$ptrcolour_rows" -eq 4 ]]
echo "GATE_PTRCOLOUR_PRODUCT_BINDING_OK rows=$ptrcolour_rows elf=$OUT/cj_gc_unit"

# The classifier's four required colour-family rows are coupled to this stable
# producer set.  A producer/anchor removal, an empty set, or a partial family
# declaration fails before the behavioral suite can lend it a green result.
PTRCOLOUR_PRODUCER_MANIFEST="$SRC/product_colour_producer_manifest.tsv"
EXPECTED_PTRCOLOUR_PRODUCERS=(store_good stale_load_bad interior_store_good bulk_store_good)
ptrcolour_producer_rows=0
while IFS=$'\t' read -r producer_name source_file stable_anchor required_families; do
  if [[ "$producer_name" == "producer_name" ]]; then
    continue
  fi
  [[ "$required_families" == "remap,marked_young,marked_old,remembered" ]]
  /usr/bin/grep -F -q "$stable_anchor" "$ROOT/$source_file"
  ptrcolour_producer_rows=$((ptrcolour_producer_rows + 1))
done <"$PTRCOLOUR_PRODUCER_MANIFEST"
[[ "$ptrcolour_producer_rows" -eq "${#EXPECTED_PTRCOLOUR_PRODUCERS[@]}" ]]
for producer_name in "${EXPECTED_PTRCOLOUR_PRODUCERS[@]}"; do
  /usr/bin/grep -q "^${producer_name}"$'\t' "$PTRCOLOUR_PRODUCER_MANIFEST"
done
echo "GATE_PTRCOLOUR_PRODUCER_MANIFEST_OK rows=$ptrcolour_producer_rows families=4"

LOADHEAL_FULL="$OUT/cj_gc_forwarding_publication_unit.full-defined.txt"
LOADHEAL_UNDEFINED="$OUT/cj_gc_forwarding_publication_unit.undefined.txt"
nm --defined-only "$OUT/cj_gc_forwarding_publication_unit" | c++filt >"$LOADHEAL_FULL"
nm -u "$OUT/cj_gc_forwarding_publication_unit" | c++filt >"$LOADHEAL_UNDEFINED"
if ! /usr/bin/grep -Eq '[[:space:]]main$' "$LOADHEAL_FULL"; then
  echo "GC_UNIT_LOADHEAL_NM_POSITIVE_CONTROL_FAIL symbol=main" >&2
  exit 8
fi
for consumer in "${LOADHEAL_PRODUCT_CONSUMERS[@]}"; do
  if /usr/bin/grep -F -q "$consumer" "$LOADHEAL_FULL"; then
    echo "GC_UNIT_LOADHEAL_LOCAL_DEFINITION symbol=$consumer" >&2
    exit 9
  fi
  if ! /usr/bin/grep -F -q "$consumer" "$LOADHEAL_UNDEFINED"; then
    echo "GC_UNIT_LOADHEAL_IMPORT_MISSING symbol=$consumer" >&2
    exit 10
  fi
done
MUTUALWAIT_SO_EXPORTS="$OUT/cj_gc_forwarding_publication_unit.so-exports.txt"
nm -D --defined-only "$RUNTIME_LIB_DIR/libcangjie-runtime.so" | c++filt >"$MUTUALWAIT_SO_EXPORTS"
for consumer in 'MapleRuntime::WCollector::FindToVersion(' 'MapleRuntime::ForwardingTable::LookupTo('; do
  if /usr/bin/grep -F -q "$consumer" "$LOADHEAL_FULL"; then
    echo "GC_UNIT_MUTUALWAIT_LOCAL_DEFINITION symbol=$consumer" >&2
    exit 9
  fi
  if ! /usr/bin/grep -F -q "$consumer" "$MUTUALWAIT_SO_EXPORTS"; then
    echo "GC_UNIT_MUTUALWAIT_PRODUCT_EXPORT_MISSING symbol=$consumer" >&2
    exit 10
  fi
done
echo "GATE_MUTUALWAIT_PRODUCT_IMPORTS_OK elf=$OUT/cj_gc_forwarding_publication_unit"
echo "GATE_LOADHEAL_PRODUCT_IMPORTS_OK elf=$OUT/cj_gc_forwarding_publication_unit"

# Registration is part of the product-shape contract: the default product must
# not register hook-dependent tests, while a complete testable product must
# register exactly this explicit target set.  Symbol inspection happens before
# execution so a missing registration cannot borrow an aggregate green tally.
for test_name in "${PUBLICATION_HOOK_TESTS[@]}"; do
  test_symbol="${test_name#*.}"
  registered=0
  if /usr/bin/grep -F -q "$test_symbol" "$LOADHEAL_FULL"; then
    registered=1
  fi
  if [[ "$PUBLICATION_HOOK_PRODUCT_SHAPE" == testable && "$registered" -ne 1 ]]; then
    echo "GC_UNIT_PUBLICATION_HOOK_TEST_NOT_REGISTERED test=$test_name" >&2
    exit 17
  fi
  if [[ "$PUBLICATION_HOOK_PRODUCT_SHAPE" == default && "$registered" -ne 0 ]]; then
    echo "GC_UNIT_PUBLICATION_HOOK_TEST_REGISTERED_FOR_DEFAULT test=$test_name" >&2
    exit 18
  fi
  if [[ "$PUBLICATION_HOOK_PRODUCT_SHAPE" == default ]]; then
    echo "NOT_RUN(default product shape) test=$test_name"
  else
    echo "GC_UNIT_PUBLICATION_HOOK_TEST_REGISTERED test=$test_name"
  fi
done

remap_receipt_symbol="${REMAP_RECEIPT_TEST#*.}"
remap_receipt_registered=0
if /usr/bin/grep -F -q "$remap_receipt_symbol" "$LOADHEAL_FULL"; then
  remap_receipt_registered=1
fi
if [[ "$REMAP_RECEIPT_PRODUCT_SHAPE" == testable && "$remap_receipt_registered" -ne 1 ]]; then
  echo "GC_UNIT_REMAP_RECEIPT_TEST_NOT_REGISTERED test=$REMAP_RECEIPT_TEST" >&2
  exit 20
fi
if [[ "$REMAP_RECEIPT_PRODUCT_SHAPE" == default && "$remap_receipt_registered" -ne 0 ]]; then
  echo "GC_UNIT_REMAP_RECEIPT_TEST_REGISTERED_FOR_DEFAULT test=$REMAP_RECEIPT_TEST" >&2
  exit 21
fi
if [[ "$REMAP_RECEIPT_PRODUCT_SHAPE" == default ]]; then
  echo "NOT_RUN(default product shape) test=$REMAP_RECEIPT_TEST"
else
  echo "GC_UNIT_REMAP_RECEIPT_TEST_REGISTERED test=$REMAP_RECEIPT_TEST"
fi

echo "LINKED_RUNTIME=$RUNTIME_LIB_DIR"
echo "MRT_TESTABLE_INTERNALS=${MRT_TESTABLE_INTERNALS:-0}"
# Binding proof: undefined product symbols must resolve from libcangjie-runtime.
if command -v nm >/dev/null 2>&1; then
  echo "=== BINDING_PROOF (undefined in binary that resolve via runtime) ==="
  nm -u "$OUT/cj_gc_unit" 2>/dev/null | grep -E 'RangeRegistry|RelocationRequestQueue|ReceiptAllowsForwarded|VerifyRoots|RouteInfo|PlausibleManagedObjectGate|TryRecoverInteriorBase|RecordCrossGen|BindLiveInfo|GetRoute|MarkGoodHeapGate' || true
  echo "=== RUNTIME_EXPORTS (product .so) ==="
  nm -D "$RUNTIME_LIB_DIR/libcangjie-runtime.so" 2>/dev/null | grep -E 'RangeRegistry|RelocationRequestQueue|ReceiptAllowsForwarded|VerifyRoots|PlausibleManagedObjectGate|TryRecoverInteriorBase|RouteInfo8GetRoute|RecordCrossGenEdge|MarkGoodHeapGate' | head -40 || true
fi

printf -v GC_UNIT_MAIN_ENV '%s\n' "${M0_CORRELATION_ENV[@]}"
GC_UNIT_MAIN_ENV=${GC_UNIT_MAIN_ENV%$'\n'}
export GC_UNIT_MAIN_ENV
set +e
bash "$SRC/run_parallel_tests.sh" \
  "$OUT/cj_gc_unit" "$OUT/cj_gc_forwarding_publication_unit" "$OUT" "$RUNTIME_LIB_DIR"
runner_rc=$?
set -e

# Keep #63's product-shape checks after replacing its whole-ELF publication run
# with per-case processes. Each publication case has an independent log, so the
# same required RUN/PASS and I03 result tokens remain observable.
publication_logs=("$OUT"/test-logs/*-publication.log)
publication_contract_rc=0
for test_name in "${PUBLICATION_HOOK_TESTS[@]}"; do
  if [[ "$PUBLICATION_HOOK_PRODUCT_SHAPE" == testable ]]; then
    if ! /usr/bin/grep -F -q "[  RUN   ] $test_name" "${publication_logs[@]}" ||
        ! /usr/bin/grep -F -q "[  PASS  ] $test_name" "${publication_logs[@]}"; then
      echo "GC_UNIT_PUBLICATION_HOOK_TEST_DID_NOT_PASS test=$test_name" >&2
      publication_contract_rc=1
    fi
  elif /usr/bin/grep -F -q "[  RUN   ] $test_name" "${publication_logs[@]}"; then
    echo "GC_UNIT_PUBLICATION_HOOK_TEST_RAN_FOR_DEFAULT test=$test_name" >&2
    publication_contract_rc=1
  fi
done
if [[ "$PUBLICATION_HOOK_PRODUCT_SHAPE" == testable ]] &&
    ! /usr/bin/grep -F -q 'I03_TARGET_REACHED state=1 count=1' "${publication_logs[@]}"; then
  echo "GC_UNIT_I03_TARGET_NOT_REACHED" >&2
  publication_contract_rc=1
fi
if [[ "$REMAP_RECEIPT_PRODUCT_SHAPE" == testable ]]; then
  if ! /usr/bin/grep -F -q "[  RUN   ] $REMAP_RECEIPT_TEST" "${publication_logs[@]}" ||
      ! /usr/bin/grep -F -q "[  PASS  ] $REMAP_RECEIPT_TEST" "${publication_logs[@]}" ||
      ! /usr/bin/grep -F -q 'TARGET_CURRENT_REMSET_ASSERT_EXECUTED' "${publication_logs[@]}"; then
    echo "GC_UNIT_REMAP_RECEIPT_TEST_DID_NOT_PASS test=$REMAP_RECEIPT_TEST" >&2
    publication_contract_rc=1
  fi
elif /usr/bin/grep -F -q "[  RUN   ] $REMAP_RECEIPT_TEST" "${publication_logs[@]}"; then
  echo "GC_UNIT_REMAP_RECEIPT_TEST_RAN_FOR_DEFAULT test=$REMAP_RECEIPT_TEST" >&2
  publication_contract_rc=1
fi
if [[ $publication_contract_rc -ne 0 ]]; then
  # The aggregate tally is completion evidence. Do not leave one consumable
  # after a post-run product-shape contract fails.
  if [[ -n "${GC_UNIT_TALLY_FILE:-}" ]]; then
    rm -f "$GC_UNIT_TALLY_FILE"
  fi
  exit 1
fi
exit "$runner_rc"
