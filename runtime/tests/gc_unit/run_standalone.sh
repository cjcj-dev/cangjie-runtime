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
      'CJ_GetUIThreadStackTop' \
      'CJ_PushUIThreadStackTop' \
      'CJ_MRT_RolveCycleRef' \
      'ResolveCycleRefStub' \
      'MapleRuntime::ZCrossVM::ResolveCycleRef()' \
      'MapleRuntime::ZCrossVM::GetCrossRefHandler(MapleRuntime::BaseObject*)' \
      'MapleRuntime::Heap::RequestGC(MapleRuntime::GCReason)' \
      'MapleRuntime::ConcurrentGCBreakpoints::RunTo(char const*)' \
      'MapleRuntime::ZDriverMajor::gc(MapleRuntime::ZDriverRequest const&)' \
      'MapleRuntime::ZCrossVM::PostResolveCycleTask()'; do
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
  cp -f "$libc_real" "$runroot/libc.so"

  if [[ -z "${GC_UNIT_OHOS_HOST_TEST_ELF:-}" ]]; then
    echo "GC_UNIT_OHOS_HOST_HEADER_ROOT=${runtime_include_flags[0]#-I}"
    "$CXX" -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti -fexceptions \
      -fvisibility-inlines-hidden -D__OHOS__=1 -DMRT_GC_UNIT_TESTS=1 \
      -DMRT_TESTABLE_INTERNALS=1 -include string \
      -I"$host_inc" -I"$SRC" -I"$ROOT/runtime/src" -I"$ROOT/runtime/src/Heap" \
      -I"$ROOT/runtime/src/Heap/z/os/linux" \
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
      'ResolveCycleRefStub' \
      'MapleRuntime::ZCrossVM::ResolveCycleRef()' \
      'MapleRuntime::ZCrossVM::GetCrossRefHandler(MapleRuntime::BaseObject*)' \
      'MapleRuntime::Heap::RequestGC(MapleRuntime::GCReason)' \
      'MapleRuntime::ConcurrentGCBreakpoints::RunTo(char const*)' \
      'MapleRuntime::ZDriverMajor::gc(MapleRuntime::ZDriverRequest const&)' \
      'MapleRuntime::ZCrossVM::PostResolveCycleTask()'; do
    if /usr/bin/grep -F -q "$symbol" "$test_nm"; then
      echo "GC_UNIT_OHOS_HOST_LOCAL_PRODUCT_DEFINITION symbol=$symbol" >&2
      return 24
    fi
  done
  for symbol in \
      'MapleRuntime::ConcurrentGCBreakpoints::RunTo(char const*)' \
      'MapleRuntime::ZCrossVM::PostResolveCycleTask()'; do
    if ! /usr/bin/grep -F -q "$symbol" "$test_undef"; then
      echo "GC_UNIT_OHOS_HOST_PRODUCT_IMPORT_MISSING symbol=$symbol" >&2
      return 25
    fi
  done

  objdump -drC "$so" | sed -n \
    '/<MapleRuntime::ZCrossVM::PostResolveCycleTask()>/,/^$/p' >"$post_disassembly"
  if ! /usr/bin/grep -F -q 'CJ_MRT_RolveCycleRef' "$post_disassembly"; then
    echo "GC_UNIT_OHOS_HOST_POST_DISPATCH_MISSING" >&2
    return 26
  fi

  sha256sum "$elf" "$so" "$bounds" >"$OUT/ohos_host_artifacts.sha256"
  # Git metadata is optional in the build service's tar snapshots. Keep
  # declarations separate from the identity captured in the linked product.
  local git_head="" git_status="" git_state=absent status_rc=0
  local source_commit="${SOURCE_COMMIT:-}" commit_origin=SOURCE_COMMIT
  local product_identity product_declared
  local provenance="$OUT/ohos_host_product.provenance.txt"
  strings "$so" >"$provenance"
  product_identity=$(sed -n 's/^CJRT-COMMIT://p' "$provenance")
  product_declared=$(sed -n 's/^CJRT-DECLARED://p' "$provenance")
  if [[ -e "$ROOT/.git" ]]; then
    git_head=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null) || git_head=""
    if [[ -n "$git_head" ]]; then
      git_state=present
      git_status=$(git -C "$ROOT" status --porcelain 2>/dev/null) || status_rc=$?
    fi
  fi
  if [[ -z "$source_commit" ]]; then
    source_commit="${CJ_RUNTIME_COMMIT:-}"
    commit_origin=CJ_RUNTIME_COMMIT
  fi
  if [[ -z "$source_commit" && "$product_declared" != none ]]; then
    source_commit="$product_declared"
    commit_origin=product-declared
  fi
  if [[ -z "$source_commit" ]]; then
    source_commit="${git_head:-unknown}"
    commit_origin=git
    [[ -n "$git_head" ]] || commit_origin=unknown
  fi
  {
    echo "BUILD_CAPTURED_AT=$(date --iso-8601=seconds)"
    echo "SOURCE_COMMIT=$source_commit"
    echo "SOURCE_COMMIT_ORIGIN=$commit_origin"
    echo "SOURCE_PRODUCT_IDENTITY=${product_identity:-unknown}"
    echo "SOURCE_PRODUCT_DECLARED=${product_declared:-unknown}"
    echo "SOURCE_GIT=$git_state"
    echo "SOURCE_GIT_HEAD=${git_head:-unknown}"
    if [[ "$git_state" == absent ]]; then
      echo "SOURCE_STATUS_RC=NOT_RUN"
    else
      echo "SOURCE_STATUS_RC=$status_rc"
    fi
    echo "SOURCE_STATUS_BEGIN"
    printf '%s\n' "$git_status"
    echo "SOURCE_STATUS_END"
  } >"$OUT/ohos_host_lineage.txt"

  declare -a tests=(
    OHOSCycle.HandlerChainThroughMajorEntry
    OHOSCycle.HandlerReceivesCurrentRootsAfterRelocate
    OHOSCycle.HandlerRefreshesRootsAfterCallbackRelocate
    OHOSCycle.MajorEntryPostsResolveTask
    OHOSCycle.PostResolvePostsProductTask
    OHOSCycle.EmptyWorkDoesNotPost
  )
  declare -a keys=(HANDLER RELOCATE CALLBACK MAJOR POST EMPTY)
  declare -a states=(NOT_RUN NOT_RUN NOT_RUN NOT_RUN NOT_RUN NOT_RUN)
  declare -a rcs=(125 125 125 125 125 125)

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
    echo "PRODUCT_RECEIPT=PostResolveCycleTask-dispatch-disassembly"
    echo "QUALIFICATION=host-product-path-only;device-ABI-NOT_RUN-773"
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
  echo "GC_UNIT_OHOS_HOST_OK filters=${#tests[@]} receipt=$receipt elf=$elf"
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

# Like HotSpot CompileGtest.gmk, inherit the linked product's compile-time
# configuration. The published recipe is bound to both selected SO hashes;
# a deleted test hook is not a configuration interface.
PRODUCT_CONFIGURATION=$(python3 "$SRC/product_test_configuration.py" \
  "$ROOT/runtime" "$RUNTIME_LIB_DIR" "$GCV2_RUNTIME_OUTPUT_ROOT")
read -r SO_TESTABLE SO_GC_UNIT_TESTS <<<"$PRODUCT_CONFIGURATION"
if [[ -n "${MRT_TESTABLE_INTERNALS:-}" && "$MRT_TESTABLE_INTERNALS" != "$SO_TESTABLE" ]]; then
  echo "GC_UNIT_PRODUCT_CONFIGURATION_MISMATCH requested=$MRT_TESTABLE_INTERNALS product=$SO_TESTABLE" >&2
  exit 2
fi
MRT_TESTABLE_INTERNALS=$SO_TESTABLE
if [[ "$SO_GC_UNIT_TESTS" == 1 ]]; then
  TEST_DEFINES+=(-DMRT_GC_UNIT_TESTS=1)
  echo "GC_UNIT_PRODUCT_CONFIGURATION=MRT_GC_UNIT_TESTS"
else
  echo "GC_UNIT_PRODUCT_CONFIGURATION=DEFAULT"
fi
nm -D "$RUNTIME_LIB_DIR/libcangjie-runtime.so" >"$OUT/runtime-dynamic-symbols.txt"
if /usr/bin/grep -Eq 'PendingStalledAllocations' \
    "$OUT/runtime-dynamic-symbols.txt"; then
  STALL_PRODUCT_OBSERVE=1
else
  STALL_PRODUCT_OBSERVE=0
fi
echo "STALL_PRODUCT_OBSERVE=$STALL_PRODUCT_OBSERVE"

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
  -I"$ROOT/runtime/src/Heap/z/os/linux"
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
  "$SRC/test_colour_address.cpp"
  "$SRC/test_zBitField.cpp"
  "$SRC/test_zBitMap.cpp"
  "$SRC/test_zRememberedSet.cpp"
  "$SRC/test_zList.cpp"
  "$SRC/test_zArray.cpp"
  "$SRC/test_zIndexDistributor.cpp"
  "$SRC/test_zValue.cpp"
  "$SRC/test_zUtils.cpp"
  "$SRC/test_used_generation.cpp"
  "$SRC/test_zstat.cpp"
  "$SRC/test_zstat_heap.cpp"
  "$SRC/test_zserviceability.cpp"
  "$SRC/test_trustp1_phase1.cpp"

  "$SRC/test_defect_regressions.cpp"
  "$SRC/test_zLiveMap.cpp"
  "$SRC/test_region_age.cpp"
  "$SRC/test_unwind_regressions.cpp"
  "$SRC/test_stackmap_base_capture.cpp"
  "$SRC/test_gctibzero.cpp"
  "$SRC/test_field_iterator.cpp"
  "$SRC/test_pinroot.cpp"
  "$SRC/test_followedge.cpp"
  "$SRC/test_colour_is_checks.cpp"
  "$SRC/test_remap_young_roots.cpp"
  "$SRC/test_zForwarding.cpp"
  "$SRC/test_z_forwarding_table.cpp"
  "$SRC/test_allocation_stall_queue.cpp"
  "$SRC/test_export_root_release.cpp"
  "$SRC/test_p05_heuristics.cpp"
  "$SRC/test_young_conc.cpp"
  "$SRC/test_alloc_buffer_handoff.cpp"
  "$SRC/test_tlab_usage.cpp"
  "$SRC/test_raw_null_allocation.cpp"
  "$SRC/test_object_allocator_paths.cpp"
  "$SRC/test_shared_small_page.cpp"
  "$SRC/test_young_weak.cpp"
  "$SRC/test_native_root_current.cpp"
  "$SRC/test_concurrency_root_color.cpp"
  "$SRC/test_relocation_set_selector.cpp"
  "$SRC/test_store_barrier_buffer.cpp"
  "$SRC/test_old_to_young_1102.cpp"
  "$SRC/test_barrier_old_atomic.cpp"
  "$SRC/test_zPageAge.cpp"
  "$SRC/test_zPage.cpp"
  "$SRC/test_value_root_identity.cpp"
  "$SRC/test_zVirtualMemory.cpp"
  "$SRC/test_zVirtualMemoryManager.cpp"
  "$SRC/test_mapped_cache.cpp"
  "$SRC/test_page_retirement.cpp"
  "$SRC/test_stay_young.cpp"
  "$SRC/test_gc_trigger.cpp"
  "$SRC/test_gc_director.cpp"
  "$SRC/test_gc_request_sync.cpp"

  "$SRC/test_string_dedup.cpp"
  "$SRC/test_concurrent_gc_breakpoints.cpp"
  "$SRC/test_runtime_config.cpp"
  "$SRC/test_uncommitter.cpp"
  "$SRC/test_gc_thread_pool.cpp"
  "$SRC/test_zWorkers.cpp"
  "$SRC/test_zGeneration.cpp"
  "$SRC/test_generation_mark_free.cpp"

  "$SRC/test_exempt_unlock.cpp"
  "$SRC/test_isfromreg.cpp"
  "$SRC/test_current_object_ref.cpp"
  "$SRC/test_i2_readref.cpp"
  "$SRC/test_loadfc.cpp"
  "$SRC/test_upstream_demangle.cpp"
  "$SRC/test_arraycopy.cpp"
  "$SRC/test_fnlz_roots.cpp"
  "$SRC/test_reference_processor.cpp"
  "$SRC/test_mark_stack_entry.cpp"
  "$SRC/test_mark_stripe.cpp"
  "$SRC/test_mark_port_203_storage.cpp"
  "$SRC/test_mark_port_203_entries.cpp"
  "$SRC/test_mark_discovery.cpp"
  "$SRC/test_mark_port_203_engine.cpp"
  "$SRC/test_partial_array.cpp"
  "$SRC/test_segmented_array_init.cpp"
  "$SRC/test_package_init.cpp"
  "$SRC/test_verify_roots.cpp"
  "$SRC/test_p10_roots_iterator.cpp"
  "$SRC/test_weak_roots.cpp"
  "$SRC/test_sync_native_wait.cpp"
  "$SRC/test_verify_fail_close.cpp"
  "$SRC/test_verify_phase.cpp"
  "$SRC/test_verify_marking_stacks.cpp"
  "$SRC/test_colour_census.cpp"
  "$SRC/test_payload_clamp.cpp"
  "$SRC/test_write_generic.cpp"
  "$SRC/test_cycle_ref_saferegion.cpp"
  "$SRC/test_thread_smr_reclaim.cpp"
)

PUBLICATION_COMPILE_FLAGS=(
  -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti
  -fvisibility-inlines-hidden
  "${TEST_DEFINES[@]}"
  -DMRT_TESTABLE_INTERNALS=1
  "${TESTABLE_FLAGS[@]}"
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
# be defined by this executable itself.  A removed visibility or
# archive-exclusion flag therefore fails closed instead of silently restoring
# the old self-satisfying weak copies.
STANDALONE_SYMBOLS=(
  _ZN12MapleRuntime8ZLiveMap5resetENS_13ZGenerationIdE
  _ZN12MapleRuntime8ZLiveMap13reset_segmentEm
)
STANDALONE_FULL_SYMBOLS=(
  _ZNK12MapleRuntime5ZPage19clone_for_promotionEv
  _ZN12MapleRuntime5ZMark15MarkEntryObjectEPNS_10BaseObjectERKNS_14MarkStackEntryEPNS_13MarkLiveCacheE
  _ZN12MapleRuntime8ZBarrier4MarkILb0ELb0ELb1ELb0EEEvNS_8zaddressE
  _ZN12MapleRuntime8ZBarrier4MarkILb0ELb0ELb0ELb0EEEvNS_8zaddressE
)
# ZPage::clone_for_promotion, ZMark entry, and both ordinary-root mark modes are out-of-line
# product functions. Full symbols exclude local copies as well as exports;
# matching product definitions keep retired names from making the guard inert.
STANDALONE_SYMBOL_DYN="$OUT/cj_gc_unit.dynamic-defined.txt"
STANDALONE_SYMBOL_FULL="$OUT/cj_gc_unit.full-defined.txt"
STANDALONE_PRODUCT_FULL="$OUT/standalone-product.full-defined.txt"
nm -D --defined-only "$OUT/cj_gc_unit" >"$STANDALONE_SYMBOL_DYN"
nm --defined-only "$OUT/cj_gc_unit" >"$STANDALONE_SYMBOL_FULL"
nm --defined-only "$RUNTIME_LIB_DIR/libcangjie-runtime.so" >"$STANDALONE_PRODUCT_FULL"
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
  if ! /usr/bin/grep -F -q "$symbol" "$STANDALONE_PRODUCT_FULL"; then
    echo "GC_UNIT_STANDALONE_PRODUCT_SYMBOL_MISSING symbol=$symbol" >&2
    exit 7
  fi
  if /usr/bin/grep -F -q "$symbol" "$STANDALONE_SYMBOL_FULL"; then
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
  [[ "$consumer" == "ZBarrier::store_barrier_on_heap_oop_field(reinterpret_cast<volatile zpointer*>(&field), false); // oldvalue-anchor" ]]
  suite="${test_name%%.*}"
  name="${test_name#*.}"
  /usr/bin/grep -F -q "GC_TEST($suite, $name)" "$SRC/test_store_barrier_buffer.cpp"
  /usr/bin/grep -F -q "$anchor" "$ROOT/runtime/src/Heap/z/zBarrier.inline.hpp"
  /usr/bin/grep -F -q "$consumer" "$SRC/test_store_barrier_buffer.cpp"
  /usr/bin/grep -F -q "$cut_site" "$ROOT/runtime/src/Heap/z/zBarrier.cpp"
  oldvalue_rows=$((oldvalue_rows + 1))
done <"$OLDVALUE_MANIFEST"
[[ "$oldvalue_rows" -eq "${#EXPECTED_OLDVALUE_TESTS[@]}" ]]
for test_name in "${EXPECTED_OLDVALUE_TESTS[@]}"; do
  /usr/bin/grep -q "^${test_name}"$'\t' "$OLDVALUE_MANIFEST"
done
[[ $(/usr/bin/grep -F -c 'oldvalue-anchor' \
  "$SRC/test_store_barrier_buffer.cpp") -eq "$oldvalue_rows" ]]
OLDVALUE_UNDEFINED="$OUT/cj_gc_unit.undefined-oldvalue.txt"
nm -u "$OUT/cj_gc_unit" >"$OLDVALUE_UNDEFINED"
if ! /usr/bin/grep -F -q 'store_barrier_on_heap_oop_field' "$ROOT/runtime/src/Heap/z/zBarrier.inline.hpp"; then
  echo "GC_UNIT_OLDVALUE_IMPORT_MISSING symbol=store_barrier_on_heap_oop_field" >&2
  exit 10
fi
if ! /usr/bin/grep -F -q 'store_barrier_on_heap_oop_field' "$OUT/runtime-dynamic-symbols.txt" &&
   ! /usr/bin/grep -F -q 'store_barrier_on_heap_oop_field' "$ROOT/runtime/src/Heap/z/zBarrier.inline.hpp"; then
  echo "GC_UNIT_OLDVALUE_EXPORT_MISSING symbol=store_barrier_on_heap_oop_field" >&2
  exit 10
fi
echo "GATE_OLDVALUE_PRODUCT_BINDING_OK rows=$oldvalue_rows elf=$OUT/cj_gc_unit"
STALL_TEST_DEFINED=$(nm --defined-only "$OUT/cj_gc_unit" | /usr/bin/grep -c 'AllocationStall_' || true)
echo "STALL_TEST_DEFINED=$STALL_TEST_DEFINED"
# The migrated tests enter Heap::alloc_page; StallAllocation is reached inside
# the product, not called directly by a test-side queue. ZGC zHeap.cpp:491.
if [[ "$STALL_TEST_DEFINED" -eq 0 ]]; then
  echo "GC_UNIT_GATE_FAIL: AllocationStall tests are missing" >&2
  exit 8
fi
nm -u "$OUT/cj_gc_unit" > "$OUT/stall-imports.txt"
if ! /usr/bin/grep -q 'alloc_page' "$OUT/stall-imports.txt"; then
  echo "GC_UNIT_GATE_FAIL: missing product alloc_page import" >&2
  exit 8
fi
echo "STALL_SUITE=PRODUCT_BOTH_CONFIGURATIONS"

# ReferenceProcessor is an independently replaceable product carrier. Guard
# full symbols (not only the dynamic table) so no local/weak test copy can
# satisfy its consumers, then require the executable to import those methods.
REFERENCE_PROCESSOR_CONSUMERS=(
  'MapleRuntime::ReferenceProcessor::discover_reference('
  'MapleRuntime::ReferenceProcessor::ProcessReferences('
  'MapleRuntime::ReferenceProcessor::EnqueueReferences('
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
    'MapleRuntime::ZGenerationYoung::pause_mark_start()'
    'MapleRuntime::ZGenerationYoung::concurrent_mark()'
    'MapleRuntime::ZGenerationOld::mark_start()'
    'MapleRuntime::ZGenerationOld::concurrent_mark()'
    'MapleRuntime::ZGenerationOld::pause_mark_end()'
    'MapleRuntime::ZGenerationOld::process_non_strong_references()'
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

# Check the macro-gated registration before an unrelated tally can qualify
# an executable with silently omitted tests. The manifest names existing
# declarations; it never registers tests or changes their assertions.
env LD_LIBRARY_PATH="$RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$OUT/cj_gc_unit" --gtest_list_tests >"$OUT/configuration-registered.raw"
python3 "$SRC/check_macro_registration.py" "$SO_GC_UNIT_TESTS" \
  "$SRC/gc_unit_macro_tests.txt" "$OUT/configuration-registered.raw"

# #711 D: LoadHealDeliveryProduct and its receipt manifest were removed with P16.

# Access API and self-heal are header templates; bind their out-of-line
# resolution and transition checks to independently replaceable product functions.  Full nm excludes even local/weak test copies; nm -u proves
# the calls are imports.  main is the positive control above.
PTRCOLOUR_PRODUCT_CONSUMERS=()
PTRCOLOUR_PRODUCT_CONSUMERS+=('MapleRuntime::ZGeneration::relocate_or_remap_object(')
PTRCOLOUR_PRODUCT_CONSUMERS+=('MapleRuntime::AssertBarrierTransitionMonotonicity(')
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
  [[ "$carrier" == "header_template" ]]
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
[[ "$ptrcolour_rows" -eq 1 ]]
echo "GATE_PTRCOLOUR_PRODUCT_BINDING_OK rows=$ptrcolour_rows elf=$OUT/cj_gc_unit"

# The classifier's four required metadata groups (old mark or finalizable) are coupled to this stable
# producer set.  A producer/anchor removal, an empty set, or a partial family
# declaration fails before the behavioral suite can lend it a green result.
PTRCOLOUR_PRODUCER_MANIFEST="$SRC/product_colour_producer_manifest.tsv"
EXPECTED_PTRCOLOUR_PRODUCERS=(store_good interior_store_good bulk_store_good finalizable_good)
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

echo "LINKED_RUNTIME=$RUNTIME_LIB_DIR"
echo "MRT_TESTABLE_INTERNALS=${MRT_TESTABLE_INTERNALS:-0}"
# Binding proof: undefined product symbols must resolve from libcangjie-runtime.
if command -v nm >/dev/null 2>&1; then
  echo "=== BINDING_PROOF (undefined in binary that resolve via runtime) ==="
  nm -u "$OUT/cj_gc_unit" 2>/dev/null | grep -E 'RangeRegistry|ZRelocateQueue|ReceiptAllowsForwarded|ZVerify|RouteInfo|RecordCrossGen|BindLiveInfo|GetRoute' || true
  echo "=== RUNTIME_EXPORTS (product .so) ==="
  nm -D "$RUNTIME_LIB_DIR/libcangjie-runtime.so" 2>/dev/null | grep -E 'RangeRegistry|ZRelocateQueue|ReceiptAllowsForwarded|ZVerify|RouteInfo8GetRoute|RecordCrossGenEdge' | head -40 || true
fi

GC_UNIT_MAIN_ENV=''
GC_UNIT_MAIN_ENV=${GC_UNIT_MAIN_ENV%$'\n'}
export GC_UNIT_MAIN_ENV
"$CXX" -std=c++17 -pthread "$SRC/test_other_vm_exit.cpp" -o "$OUT/cj_gc_other_vm_exit_unit"
sha256sum "$OUT/cj_gc_other_vm_exit_unit" > "$OUT/other_vm_exit.sha256"
set +e
"$OUT/cj_gc_other_vm_exit_unit" > "$OUT/other_vm_exit.log" 2>&1
other_vm_exit_rc=$?
cat "$OUT/other_vm_exit.log"
echo "GC_UNIT_OTHER_VM_EXIT_RC=$other_vm_exit_rc"
bash "$SRC/run_other_vm_teardown.sh" "$OUT/cj_gc_unit" "$RUNTIME_LIB_DIR" "$OUT"
teardown_rc=$?
echo "GC_UNIT_OTHER_VM_TEARDOWN_RC=$teardown_rc"
bash "$SRC/run_parallel_tests.sh" \
  "$OUT/cj_gc_unit" "$OUT/cj_gc_forwarding_publication_unit" "$OUT" "$RUNTIME_LIB_DIR"
runner_rc=$?
set -e

if [[ "$other_vm_exit_rc" -ne 0 ]]; then exit "$other_vm_exit_rc"; fi
if [[ "$teardown_rc" -ne 0 ]]; then exit "$teardown_rc"; fi
exit "$runner_rc"
