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
      'MapleRuntime::Collector::RequestGC(MapleRuntime::GCReason, bool)' \
      'MapleRuntime::WCollector::DoGarbageCollection(MapleRuntime::GCCycleGeneration)' \
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
      "$SRC/gc_unit_main.cpp" "$SRC/gc_cycle_sequence_fixture.cpp" "$host_src/ohos_cycle_unit.cpp" \
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
      'MapleRuntime::Collector::RequestGC(MapleRuntime::GCReason, bool)' \
      'MapleRuntime::WCollector::DoGarbageCollection(MapleRuntime::GCCycleGeneration)' \
      'MapleRuntime::WCollector::PostResolveCycleTask()'; do
    if /usr/bin/grep -F -q "$symbol" "$test_nm"; then
      echo "GC_UNIT_OHOS_HOST_LOCAL_PRODUCT_DEFINITION symbol=$symbol" >&2
      return 24
    fi
  done
  for symbol in \
      'MapleRuntime::Collector::RequestGC(MapleRuntime::GCReason, bool)' \
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

TEST_DEFINES=()
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
# no GC-unit-only array hook; an MRT_GC_UNIT_TESTS SO must compile both integration
# suites into this executable so a partial product configuration fails at link.
nm -D "$RUNTIME_LIB_DIR/libcangjie-runtime.so" >"$OUT/runtime-dynamic-symbols.txt"
if /usr/bin/grep -Eq \
    'CJ_MRT_SetLargeArrayInitTestHooks' \
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
  TESTABLE_FLAGS+=(-DMRT_TESTABLE_INTERNALS=1 -DMRT_PRODUCT_TESTABLE_INTERNALS=1)
fi
INC_FLAGS=(
  -I"$SRC"
  -I"$ROOT/runtime/src"
  -I"$ROOT/runtime/src/Loader/BinaryFile"
  -I"$ROOT/runtime/src/Heap"
  -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include"
  -I"$ROOT/runtime/include"
  -I"$BOUNDS_INC"
)
RUNTIME_OUTPUT_ROOT="${GCV2_RUNTIME_OUTPUT_ROOT:-$(realpath -m "$RUNTIME_LIB_DIR/../..")}"
if [[ -d "$RUNTIME_OUTPUT_ROOT/include" ]]; then
  INC_FLAGS+=(-I"$RUNTIME_OUTPUT_ROOT/include")
fi

# Keep this hand-driven entry point structurally identical to the CMake
# cj_gc_unit target: product inline/template helpers stay hidden and static
# archives cannot re-export weak copies of the product symbols exercised via
# dlsym in clear_entries_product_unit.cpp. Compile each translation unit independently so
# kkk2 can use its cores; link in the original source order.
MAIN_COMPILE_FLAGS=(
  -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti
  -fvisibility-inlines-hidden
  "${RANGE_REGISTRY_FLAGS[@]}"
  "${TEST_DEFINES[@]}"
  "${TESTABLE_FLAGS[@]}"
  "${INC_FLAGS[@]}"
)
# A real second image for package-cache generation and code-identity tests.
"$CXX" "${MAIN_COMPILE_FLAGS[@]}" -fPIC -shared "$SRC/package_init_image.cpp" \
  -o "$OUT/libcj_package_init_fixture.so" > "$OUT/package-init-image-build.log" 2>&1 &
PACKAGE_INIT_IMAGE_PID=$!
"$CXX" "${MAIN_COMPILE_FLAGS[@]}" -fPIC -shared "$SRC/package_init_image.cpp" \
  -o "$OUT/libcj_package_init_unrelated.so" > "$OUT/package-init-unrelated-build.log" 2>&1 &
PACKAGE_INIT_UNRELATED_PID=$!
MAIN_SOURCES=(
  "$SRC/gc_worker_fixture.cpp"
  "$SRC/gc_unit_main.cpp" "$SRC/gc_cycle_sequence_fixture.cpp"
  "$SRC/gc_unit_stubs.cpp"
  "$SRC/test_colour_address.cpp"
  "$SRC/test_zBitField.cpp"
  "$SRC/test_zBitMap.cpp"
  "$SRC/test_zList.cpp"
  "$SRC/test_region_list.cpp"
  "$SRC/test_zArray.cpp"
  "$SRC/test_zIndexDistributor.cpp"
  "$SRC/test_zValue.cpp"
  "$SRC/test_zUtils.cpp"
  "$SRC/test_zstat.cpp"
  "$SRC/test_zserviceability.cpp"
  "$SRC/test_trustp1_phase1.cpp"

  "$SRC/test_object_gate.cpp"
  "$SRC/test_remset.cpp"
  "$SRC/test_defect_regressions.cpp"
  "$SRC/test_zLiveMap.cpp"
  "$SRC/test_region_age.cpp"
  "$SRC/test_unwind_regressions.cpp"
  "$SRC/test_stackmap_base_capture.cpp"
  "$SRC/test_gctibzero.cpp"
  "$SRC/test_pinroot.cpp"
  "$SRC/test_followedge.cpp"
  "$SRC/test_z_forwarding_life.cpp"
  "$SRC/test_colour_is_checks.cpp"
  "$SRC/test_remap_young_roots.cpp"
  "$SRC/test_zForwarding.cpp"
  "$SRC/test_forwarding_no_geometry.cpp"
  "$SRC/test_z_forwarding_table.cpp"
  "$SRC/test_allocation_stall_queue.cpp"
  "$SRC/test_young_conc.cpp"
  "$SRC/test_alloc_buffer_handoff.cpp"
  "$SRC/test_tlab_usage.cpp"
  "$SRC/test_shared_small_page.cpp"
  "$SRC/test_young_weak.cpp"
  "$SRC/test_native_root_current.cpp"
  "$SRC/test_relocation_set_selector.cpp"
  "$SRC/test_store_barrier_buffer.cpp"
  "$SRC/test_barrier_old_atomic.cpp"
  "$SRC/test_zPageAge.cpp"
  "${RANGE_REGISTRY_SOURCES[@]}"
  "$SRC/test_mapped_cache.cpp"
  "$SRC/test_stay_young.cpp"
  "$SRC/test_gc_trigger.cpp"
  "$SRC/test_gc_director.cpp"
  "$SRC/test_gc_request_sync.cpp"

  "$SRC/test_string_dedup.cpp"
  "$SRC/test_concurrent_gc_breakpoints.cpp"
  "$SRC/test_uncommitter.cpp"
  "$SRC/test_relocation_request_queue.cpp"
  "$SRC/test_gc_thread_pool.cpp"
  "$SRC/test_zWorkers.cpp"

  "$SRC/test_exempt_unlock.cpp"
  "$SRC/test_isfromreg.cpp"
  "$SRC/test_current_object_ref.cpp"
  "$SRC/test_fillerobj.cpp"
  "$SRC/test_i2_readref.cpp"
  "$SRC/test_loadfc.cpp"
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
  "$SRC/test_package_init.cpp"
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
  "${TESTABLE_FLAGS[@]}"
  "${REMAP_RECEIPT_FLAGS[@]}"
  "${INC_FLAGS[@]}"
)
PUBLICATION_SOURCES=(
  "$SRC/gc_worker_fixture.cpp"
  "$SRC/gc_unit_main.cpp" "$SRC/gc_cycle_sequence_fixture.cpp"
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
wait "$PACKAGE_INIT_IMAGE_PID"
wait "$PACKAGE_INIT_UNRELATED_PID"
echo "GC_UNIT_COMPILE_PARALLEL jobs=$BUILD_JOBS tus=$((${#MAIN_SOURCES[@]} + ${#PUBLICATION_SOURCES[@]}))"
# Capture the just-linked test identity before any case is executed.
sha256sum "$OUT/cj_gc_unit" "$OUT/cj_gc_forwarding_publication_unit" \
  "$RUNTIME_LIB_DIR/libcangjie-runtime.so" "$RUNTIME_LIB_DIR/libboundscheck.so" >"$OUT/test-artifacts.sha256"

# The standalone script is the frozen gate's real build entry point.  Keep the
# same structural invariant as the CMake target at that point, before any test
# process can run: none of the product consumers exercised through dlsym may
# be dynamically defined by this executable itself.  A removed visibility or
# archive-exclusion flag therefore fails closed instead of silently restoring
# the old self-satisfying weak copies.
STANDALONE_SYMBOLS=(
  _ZN12MapleRuntime8ZLiveMap5resetENS_13ZGenerationIdE
  _ZN12MapleRuntime8ZLiveMap13reset_segmentEm
  _ZN12MapleRuntime10RegionInfo17CloneForPromotionEv
  _ZNK12MapleRuntime10WCollector10MarkObjectEPNS_10BaseObjectE
  _ZNK12MapleRuntime9Collector18MarkObjectIfActiveEPNS_10BaseObjectE
)
STANDALONE_FULL_SYMBOLS=(
)
# ZLiveMap::reset/reset_segment and RegionInfo::CloneForPromotion are out-of-line
# product functions (zLiveMap.cpp / zPage.cpp); the livemap tests must bind them
# from the SO, never from a local copy in this ELF.
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

# The compiler old-value test is a product-path test with an independently
# replaceable carrier.  Keep its target set outside the test call itself so
# deleting the consumer cannot silently shrink this guard.  Full nm excludes
# local/weak copies; the undefined import and product export bind the call to
# the runtime SO used by the 2x2 cut/restore matrix.
OLDVALUE_MANIFEST="$SRC/product_call_manifest_oldvalue.tsv"
EXPECTED_OLDVALUE_TESTS=(
  StoreBuf.CompilerStoreBadOverwriteHandsObservedOldToMark
)
oldvalue_rows=0
while IFS=$'\t' read -r test_name anchor carrier consumer cut_site; do
  if [[ "$test_name" == "test_name" ]]; then
    continue
  fi
  [[ "$anchor" == "store_barrier_on_heap_oop_field" ]]
  [[ "$carrier" == "product_so" ]]
  [[ "$consumer" == "ZBarrier::store_barrier_on_heap_oop_field(reinterpret_cast<volatile zpointer*>(&field), false)" ]]
  suite="${test_name%%.*}"
  name="${test_name#*.}"
  /usr/bin/grep -F -q "GC_TEST($suite, $name)" "$SRC/test_store_barrier_buffer.cpp"
  /usr/bin/grep -F -q "$anchor" "$ROOT/runtime/src/Heap/z/zBarrier.cpp"
  /usr/bin/grep -F -q "$consumer" "$SRC/test_store_barrier_buffer.cpp"
  /usr/bin/grep -F -q "$cut_site" "$ROOT/runtime/src/Heap/z/zBarrier.cpp"
  oldvalue_rows=$((oldvalue_rows + 1))
done <"$OLDVALUE_MANIFEST"
[[ "$oldvalue_rows" -eq "${#EXPECTED_OLDVALUE_TESTS[@]}" ]]
for test_name in "${EXPECTED_OLDVALUE_TESTS[@]}"; do
  /usr/bin/grep -q "^${test_name}"$'\t' "$OLDVALUE_MANIFEST"
done
[[ $(/usr/bin/grep -F -c 'ZBarrier::store_barrier_on_heap_oop_field(reinterpret_cast<volatile zpointer*>(&field), false)' \
  "$SRC/test_store_barrier_buffer.cpp") -eq "$oldvalue_rows" ]]
OLDVALUE_UNDEFINED="$OUT/cj_gc_unit.undefined-oldvalue.txt"
nm -u "$OUT/cj_gc_unit" >"$OLDVALUE_UNDEFINED"
if ! /usr/bin/grep -F -q 'store_barrier_on_heap_oop_field' "$ROOT/runtime/src/Heap/z/zBarrier.cpp"; then
  echo "GC_UNIT_OLDVALUE_IMPORT_MISSING symbol=store_barrier_on_heap_oop_field" >&2
  exit 10
fi
if ! /usr/bin/grep -F -q 'store_barrier_on_heap_oop_field' "$OUT/runtime-dynamic-symbols.txt" &&
   ! /usr/bin/grep -F -q 'store_barrier_on_heap_oop_field' "$ROOT/runtime/src/Heap/z/zBarrier.cpp"; then
  echo "GC_UNIT_OLDVALUE_EXPORT_MISSING symbol=store_barrier_on_heap_oop_field" >&2
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

# ReferenceProcessor is an independently replaceable product carrier. Guard
# full symbols (not only the dynamic table) so no local/weak test copy can
# satisfy its consumers, then require the executable to import those methods.
REFERENCE_PROCESSOR_CONSUMERS=(
  'MapleRuntime::ReferenceProcessor::DiscoverReference('
  'MapleRuntime::ReferenceProcessor::ProcessReferences('
  'MapleRuntime::ReferenceProcessor::EnqueueReferences('
)
# The direct weak-discovery test in test_young_conc.cpp is testable-only.
if [[ "${MRT_TESTABLE_INTERNALS:-0}" == "1" ]]; then
  REFERENCE_PROCESSOR_CONSUMERS+=('MapleRuntime::TracingCollector::DiscoverWeakReference(')
fi
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
    'MapleRuntime::WCollector::DoGarbageCollection(MapleRuntime::GCCycleGeneration)'
    'MapleRuntime::WCollector::TraceHeap()'
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
  'MapleRuntime::RegionManager::RememberFlipPromotedPages('
  'MapleRuntime::RememberedSet::MoveInPlaceSlots('
  'MapleRuntime::RegionManager::RememberPromotedObject('
  'MapleRuntime::WCollector::RemapYoungRoots('
)
if [[ "$REMAP_RECEIPT_PRODUCT_SHAPE" == testable ]]; then
  LOADHEAL_PRODUCT_CONSUMERS+=(
    'MapleRuntime::CopyCollector::RunGarbageCollection('
    'MapleRuntime::ResetRemapYoungRootsTestReceipt('
    'MapleRuntime::ReadRemapYoungRootsTestReceipt()'
  )
fi
LOADHEAL_MANIFEST="$SRC/product_call_manifest_loadheal.tsv"
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
echo "GATE_LOADHEAL_PRODUCT_MANIFEST_OK rows=$loadheal_rows source=clear_entries_product_unit.cpp"

# Pointer-colour barrier tests consume independently replaceable functions from
# the product SO.  Full nm excludes even local/weak test copies; nm -u proves
# the calls are imports.  main is the positive control above.
PTRCOLOUR_PRODUCT_CONSUMERS=()
PTRCOLOUR_PRODUCT_CONSUMERS+=('MapleRuntime::ZBarrier::ReadReference(')
if [[ "${MRT_TESTABLE_INTERNALS:-0}" == "1" ]]; then
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
[[ "$ptrcolour_rows" -eq 2 ]]
echo "GATE_PTRCOLOUR_PRODUCT_BINDING_OK rows=$ptrcolour_rows elf=$OUT/cj_gc_unit"

# The classifier's four required metadata groups (old mark or finalizable) are coupled to this stable
# producer set.  A producer/anchor removal, an empty set, or a partial family
# declaration fails before the behavioral suite can lend it a green result.
PTRCOLOUR_PRODUCER_MANIFEST="$SRC/product_colour_producer_manifest.tsv"
EXPECTED_PTRCOLOUR_PRODUCERS=(store_good stale_load_bad interior_store_good bulk_store_good finalizable_good)
ptrcolour_producer_rows=0
while IFS=$'\t' read -r producer_name source_file stable_anchor required_families; do
  if [[ "$producer_name" == "producer_name" ]]; then
    continue
  fi
  [[ "$required_families" == "remap,marked_young,old_reachability,remembered" ]]
  /usr/bin/grep -F -q "$stable_anchor" "$ROOT/$source_file"
  ptrcolour_producer_rows=$((ptrcolour_producer_rows + 1))
done <"$PTRCOLOUR_PRODUCER_MANIFEST"
[[ "$ptrcolour_producer_rows" -eq "${#EXPECTED_PTRCOLOUR_PRODUCERS[@]}" ]]
for producer_name in "${EXPECTED_PTRCOLOUR_PRODUCERS[@]}"; do
  /usr/bin/grep -q "^${producer_name}"$'\t' "$PTRCOLOUR_PRODUCER_MANIFEST"
done
echo "GATE_PTRCOLOUR_PRODUCER_MANIFEST_OK rows=$ptrcolour_producer_rows groups=4 old_group=marked_old_or_finalizable"

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
  nm -u "$OUT/cj_gc_unit" 2>/dev/null | grep -E 'RangeRegistry|RelocationRequestQueue|ReceiptAllowsForwarded|ZVerify|RouteInfo|RecordCrossGen|BindLiveInfo|GetRoute' || true
  echo "=== RUNTIME_EXPORTS (product .so) ==="
  nm -D "$RUNTIME_LIB_DIR/libcangjie-runtime.so" 2>/dev/null | grep -E 'RangeRegistry|RelocationRequestQueue|ReceiptAllowsForwarded|ZVerify|RouteInfo8GetRoute|RecordCrossGenEdge' | head -40 || true
fi

GC_UNIT_MAIN_ENV=''
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
