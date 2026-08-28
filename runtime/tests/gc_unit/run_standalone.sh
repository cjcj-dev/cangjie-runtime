#!/usr/bin/env bash
# Standalone build+run of GC unit tests.
# Links product libcangjie-runtime so U3–U7 call product symbols (not models).
# Usage:
#   GCV2_RUNTIME_LIB_DIR=/path/to/lib/x86_64_Release bash runtime/tests/gc_unit/run_standalone.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit"
OUT="${GC_UNIT_OUT:-$ROOT/runtime/tests/gc_unit/build_standalone}"
mkdir -p "$OUT"
CXX="${CXX:-clang++}"

RUNTIME_LIB_DIR="${GCV2_RUNTIME_LIB_DIR:-}"
if [[ -z "$RUNTIME_LIB_DIR" ]]; then
  for cand in \
    "$ROOT/runtime/output/temp/lib/x86_64_Release" \
    "$ROOT/runtime/output/temp/lib/x86_64_Relwithdebinfo"; do
    if [[ -f "$cand/libcangjie-runtime.so" ]]; then
      RUNTIME_LIB_DIR="$cand"
      break
    fi
  done
fi
if [[ -z "$RUNTIME_LIB_DIR" || ! -f "$RUNTIME_LIB_DIR/libcangjie-runtime.so" ]]; then
  echo "error: set GCV2_RUNTIME_LIB_DIR to a dir containing libcangjie-runtime.so" >&2
  exit 2
fi

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
M0_TEST_ARGS=()
M0_TEST_ACCESS=off
if nm -D --defined-only "$RUNTIME_LIB_DIR/libcangjie-runtime.so" 2>/dev/null | c++filt |
    /usr/bin/grep 'M0ExitDiagnostics::GetCounts' >/dev/null; then
  M0_TEST_ARGS=(-DMRT_GC_UNIT_TEST_ACCESS=1 "$SRC/test_m0_exit.cpp")
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
if [[ -d "$ROOT/runtime/output/temp/include" ]]; then
  INC_FLAGS+=(-I"$ROOT/runtime/output/temp/include")
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
# dlsym in test_live_map.cpp.
$CXX -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti \
  -fvisibility-inlines-hidden \
  "${RANGE_REGISTRY_FLAGS[@]}" \
  "${TEST_DEFINES[@]}" \
  "${TESTABLE_FLAGS[@]}" \
  "${INC_FLAGS[@]}" \
  "$SRC/gc_unit_main.cpp" \
  "$SRC/gc_unit_stubs.cpp" \
  "$ROOT/runtime/src/Base/ZStat.cpp" \
  "$SRC/test_colour_address.cpp" \
  "$SRC/test_z_bit_field.cpp" \
  "$SRC/test_z_list.cpp" \
  "$SRC/test_zstat.cpp" \
  "$SRC/test_trustp1_phase1.cpp" \
  "$SRC/test_route_info.cpp" \
  "$SRC/test_live_map.cpp" \
  "$SRC/test_object_gate.cpp" \
  "$SRC/test_remset.cpp" \
  "$SRC/test_defect_regressions.cpp" \
  "$SRC/test_region_bitmap.cpp" \
  "$SRC/test_region_age.cpp" \
  "$SRC/test_unwind_regressions.cpp" \
  "$SRC/test_gctibzero.cpp" \
  "$SRC/test_pinroot.cpp" \
  "$SRC/test_followedge.cpp" \
  "$SRC/test_z_forwarding_life.cpp" \
  "$SRC/test_colour_is_checks.cpp" \
  "$SRC/test_remap_young_roots.cpp" \
  "$SRC/test_forwarding_entries.cpp" \
  "$SRC/test_forwarding_no_geometry.cpp" \
  "$SRC/test_z_forwarding_table.cpp" \
  "$SRC/test_allocation_stall_queue.cpp" \
    "$SRC/test_young_conc.cpp" \
    "$SRC/test_relocation_set_selector.cpp" \
    "$SRC/test_store_barrier_buffer.cpp" \
    "$SRC/test_page_age.cpp" \
    "${RANGE_REGISTRY_SOURCES[@]}" \
    "$SRC/test_stay_young.cpp" \
    "$SRC/test_gc_trigger.cpp" \
    "$SRC/test_gc_request_sync.cpp" \
    "$SRC/test_mutator_relocate.cpp" \
    "$SRC/test_uncommitter.cpp" \
    "$SRC/test_relocation_request_queue.cpp" \
    "$SRC/test_gc_thread_pool.cpp" \
    "$SRC/test_expire_kept.cpp" \
    "$SRC/test_receipt_life.cpp" \
    "$SRC/test_lifeclock.cpp" \
    "$SRC/test_exempt_unlock.cpp" \
    "$SRC/test_heal_coverage.cpp" \
    "$SRC/test_diag_gate.cpp" \
    "$SRC/test_interior_edge_class.cpp" \
    "$SRC/test_isfromreg.cpp" \
     "$SRC/test_current_object_ref.cpp" \
     "$SRC/test_fillerobj.cpp" \
    "$SRC/test_i2_readref.cpp" \
    "${M0_TEST_ARGS[@]}" \
    "$SRC/test_loadfc.cpp" \
    "${M0_CORRELATION_TEST_ARGS[@]}" \
    "$SRC/test_fwdreturn.cpp" \
    "$SRC/test_ghost_region_lookup.cpp" \
    "$SRC/test_fnlz_roots.cpp" \
    "$SRC/test_reference_processor.cpp" \
    "$SRC/test_mark_stack_entry.cpp" \
    "$SRC/test_mark_stripe.cpp" \
    "$SRC/test_partial_array.cpp" \
    "$SRC/test_segmented_array_init.cpp" \
    "$SRC/test_verify_roots.cpp" \
    "$SRC/test_verify_phase.cpp" \
    "$SRC/test_mem_map.cpp" \
  -L"$RUNTIME_LIB_DIR" -Wl,-rpath,"$RUNTIME_LIB_DIR" -Wl,--exclude-libs,ALL \
  -lcangjie-runtime -lboundscheck \
  -o "$OUT/cj_gc_unit"

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
  LiveMap.BoundedPreserveProductRepairsCurrentFace
  LiveMap.BoundedPreserveProductRepairsFromPageFace
  LiveMap.ExaminedPageWithoutSnapshotStillAborts
  LiveMap.BoundedPreserveProductRepairsOwnedCopy
)
manifest_rows=0
while IFS=$'\t' read -r test_name anchor carrier consumer cut_site; do
  if [[ "$test_name" == "test_name" ]]; then
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

# Fresh-process product-link arm for the one-shot ForwardingTable.  It binds
# CompactRegion/ClearEntries from the same runtime SO as the full suite; no
# forwarding component is rebuilt into this executable.
$CXX -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti \
  -fvisibility-inlines-hidden \
  "${TEST_DEFINES[@]}" \
  -DMRT_TESTABLE_INTERNALS=1 \
  "${PUBLICATION_TESTABLE_FLAGS[@]}" \
  "${INC_FLAGS[@]}" \
  "$SRC/gc_unit_main.cpp" \
  "$SRC/clear_entries_product_unit.cpp" \
  -L"$RUNTIME_LIB_DIR" -Wl,-rpath,"$RUNTIME_LIB_DIR" -Wl,--exclude-libs,ALL \
  -lcangjie-runtime -lboundscheck \
  -o "$OUT/cj_gc_forwarding_publication_unit"

echo "LINKED_RUNTIME=$RUNTIME_LIB_DIR"
echo "MRT_TESTABLE_INTERNALS=${MRT_TESTABLE_INTERNALS:-0}"
# Binding proof: undefined product symbols must resolve from libcangjie-runtime.
if command -v nm >/dev/null 2>&1; then
  echo "=== BINDING_PROOF (undefined in binary that resolve via runtime) ==="
  nm -u "$OUT/cj_gc_unit" 2>/dev/null | grep -E 'RangeRegistry|RelocationRequestQueue|ReceiptAllowsForwarded|VerifyRoots|RouteInfo|PlausibleManagedObjectGate|TryRecoverInteriorBase|RecordCrossGen|BindLiveInfo|GetRoute|MarkGoodHeapGate' || true
  echo "=== RUNTIME_EXPORTS (product .so) ==="
  nm -D "$RUNTIME_LIB_DIR/libcangjie-runtime.so" 2>/dev/null | grep -E 'RangeRegistry|RelocationRequestQueue|ReceiptAllowsForwarded|VerifyRoots|PlausibleManagedObjectGate|TryRecoverInteriorBase|RouteInfo8GetRoute|RecordCrossGenEdge|MarkGoodHeapGate' | head -40 || true
fi

START=$(date +%s%N)
FINAL_TALLY="${GC_UNIT_TALLY_FILE:-}"
MAIN_TALLY="$OUT/main_tally.txt"
PUBLICATION_TALLY="$OUT/forwarding_publication_tally.txt"
rm -f "$MAIN_TALLY" "$PUBLICATION_TALLY"
set +e
env "${M0_CORRELATION_ENV[@]}" GC_UNIT_TALLY_FILE="$MAIN_TALLY" \
  LD_LIBRARY_PATH="$RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$OUT/cj_gc_unit"
MAIN_RC=$?
GC_UNIT_TALLY_FILE="$PUBLICATION_TALLY" \
  LD_LIBRARY_PATH="$RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$OUT/cj_gc_forwarding_publication_unit"
PUBLICATION_RC=$?
set -e
END=$(date +%s%N)
ELAPSED_MS=$(( (END - START) / 1000000 ))

# The gate consumes one independent tally.  Merge only two complete runner
# tallies; an abort or disconnect leaves the final tally absent and fails
# closed instead of turning an incomplete run into "one red".
if [[ -n "$FINAL_TALLY" && -f "$MAIN_TALLY" && -f "$PUBLICATION_TALLY" ]]; then
  read -r main_tests main_pass main_fail < <(
    sed -nE 's/^\[========\] ([0-9]+) tests: ([0-9]+) passed, ([0-9]+) failed$/\1 \2 \3/p' "$MAIN_TALLY")
  read -r publication_tests publication_pass publication_fail < <(
    sed -nE 's/^\[========\] ([0-9]+) tests: ([0-9]+) passed, ([0-9]+) failed$/\1 \2 \3/p' "$PUBLICATION_TALLY")
  if [[ -n "${main_tests:-}" && -n "${publication_tests:-}" ]]; then
    printf '[========] %d tests: %d passed, %d failed\n' \
      "$((main_tests + publication_tests))" \
      "$((main_pass + publication_pass))" \
      "$((main_fail + publication_fail))" >"$FINAL_TALLY"
  fi
fi

RC=0
if [[ $MAIN_RC -ne 0 || $PUBLICATION_RC -ne 0 ]]; then
  RC=1
fi
echo "GC_UNIT_RUN_DONE rc=$RC main_rc=$MAIN_RC publication_rc=$PUBLICATION_RC wall_ms=$ELAPSED_MS"
exit "$RC"
