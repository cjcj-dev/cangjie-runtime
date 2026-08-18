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

BOUNDS_INC="$ROOT/runtime/third_party/third_party_bounds_checking_function/include"
INC_FLAGS=(
  -I"$SRC"
  -I"$ROOT/runtime/src"
  -I"$ROOT/runtime/src/Heap"
  -I"$ROOT/runtime/include"
  -I"$BOUNDS_INC"
)
if [[ -d "$ROOT/runtime/output/temp/include" ]]; then
  INC_FLAGS+=(-I"$ROOT/runtime/output/temp/include")
fi

$CXX -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti \
  "${INC_FLAGS[@]}" \
  "$SRC/gc_unit_main.cpp" \
  "$SRC/gc_unit_stubs.cpp" \
  "$ROOT/runtime/src/Heap/Allocator/ForwardingTable.cpp" \
  "$SRC/test_colour_address.cpp" \
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
    "$SRC/test_z_forwarding_life.cpp" \
    "$SRC/test_remap_young_roots.cpp" \
  "$SRC/test_forwarding_entries.cpp" \
    "$SRC/test_z_forwarding_life.cpp" \
    "$SRC/test_young_conc.cpp" \
    "$SRC/test_store_barrier_buffer.cpp" \
  -L"$RUNTIME_LIB_DIR" -Wl,-rpath,"$RUNTIME_LIB_DIR" \
  -lcangjie-runtime -lboundscheck \
  -o "$OUT/cj_gc_unit"

echo "LINKED_RUNTIME=$RUNTIME_LIB_DIR"
# Binding proof: undefined product symbols must resolve from libcangjie-runtime.
if command -v nm >/dev/null 2>&1; then
  echo "=== BINDING_PROOF (undefined in binary that resolve via runtime) ==="
  nm -u "$OUT/cj_gc_unit" 2>/dev/null | grep -E 'RouteInfo|PlausibleManagedObjectGate|TryRecoverInteriorBase|RecordCrossGen|BindLiveInfo|GetRoute|MarkGoodHeapGate' || true
  echo "=== RUNTIME_EXPORTS (product .so) ==="
  nm -D "$RUNTIME_LIB_DIR/libcangjie-runtime.so" 2>/dev/null | grep -E 'PlausibleManagedObjectGate|TryRecoverInteriorBase|RouteInfo8GetRoute|RecordCrossGenEdge|MarkGoodHeapGate' | head -20 || true
fi

START=$(date +%s%N)
LD_LIBRARY_PATH="$RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$OUT/cj_gc_unit"
RC=$?
END=$(date +%s%N)
ELAPSED_MS=$(( (END - START) / 1000000 ))
echo "GC_UNIT_OK rc=$RC wall_ms=$ELAPSED_MS"
exit "$RC"
