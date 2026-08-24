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
    'ShouldWaitForIgnoredGcRequest|CJ_MRT_SetLargeArrayInitTestHooks' \
    "$OUT/runtime-dynamic-symbols.txt"; then
  TEST_DEFINES+=(-DMRT_GC_UNIT_TESTS=1)
  echo "GC_UNIT_PRODUCT_CONFIGURATION=MRT_GC_UNIT_TESTS"
else
  echo "GC_UNIT_PRODUCT_CONFIGURATION=DEFAULT"
fi

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

$CXX -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti \
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
    "$SRC/test_young_conc.cpp" \
    "$SRC/test_relocation_set_selector.cpp" \
    "$SRC/test_store_barrier_buffer.cpp" \
    "$SRC/test_page_age.cpp" \
    "${RANGE_REGISTRY_SOURCES[@]}" \
    "$SRC/test_stay_young.cpp" \
    "$SRC/test_gc_trigger.cpp" \
    "$SRC/test_gc_request_sync.cpp" \
    "$SRC/test_mutator_relocate.cpp" \
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
    "$SRC/test_fwdreturn.cpp" \
    "$SRC/test_ghost_region_lookup.cpp" \
    "$SRC/test_fnlz_roots.cpp" \
    "$SRC/test_mark_stack_entry.cpp" \
    "$SRC/test_mark_stripe.cpp" \
    "$SRC/test_partial_array.cpp" \
    "$SRC/test_segmented_array_init.cpp" \
    "$SRC/test_verify_roots.cpp" \
    "$SRC/test_mem_map.cpp" \
  -L"$RUNTIME_LIB_DIR" -Wl,-rpath,"$RUNTIME_LIB_DIR" \
  -lcangjie-runtime -lboundscheck \
  -o "$OUT/cj_gc_unit"

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
set +e
LD_LIBRARY_PATH="$RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$OUT/cj_gc_unit"
RC=$?
set -e
END=$(date +%s%N)
ELAPSED_MS=$(( (END - START) / 1000000 ))
echo "GC_UNIT_RUN_DONE rc=$RC wall_ms=$ELAPSED_MS"
exit "$RC"
