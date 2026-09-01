#!/usr/bin/env bash
set -uo pipefail

readonly REPO=/root/impl_gcunit3-aa48-run
readonly BASELINE=aa48a3cec46a97076718a6f5da6cc1d77411a48b
readonly BUILD="$REPO/runtime/CMakebuild_gcunit3"
readonly EVIDENCE="$REPO/evidence/impl_gcunit3/baseline"
readonly OUT="$REPO/runtime/tests/gc_unit/build_impl_gcunit3"
readonly CORES=16-31

if [[ "$(git -C "$REPO" rev-parse HEAD)" != "$BASELINE" ]] ||
    [[ -n "$(git -C "$REPO" status --porcelain --untracked-files=no)" ]]; then
    echo "baseline source identity mismatch" >&2
    exit 74
fi

cd "$REPO/runtime" || exit $?
taskset -c "$CORES" cmake -S . -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCJ_SDK_VERSION=0.0.1 \
    -DDISABLE_VERSION_CHECK=ON \
    -DMRT_GC_UNIT_TESTS=ON \
    -DMRT_TESTABLE_INTERNALS=ON \
    >"$EVIDENCE/configure-valid.log" 2>&1
configure_rc=$?
printf '%s\n' "$configure_rc" >"$EVIDENCE/configure-valid.rc"
if [[ $configure_rc -ne 0 ]]; then
    exit "$configure_rc"
fi

GC_UNIT_GATE_SKIP=1 taskset -c "$CORES" cmake --build "$BUILD" \
    --target cangjie-runtime --parallel 16 >"$EVIDENCE/build.log" 2>&1
build_rc=$?
printf '%s\n' "$build_rc" >"$EVIDENCE/build.rc"
if [[ $build_rc -ne 0 ]]; then
    exit "$build_rc"
fi

libdir="$REPO/runtime/output/temp/lib/x86_64_Release"
sha256sum "$libdir/libcangjie-runtime.so" "$libdir/libboundscheck.so" \
    >"$EVIDENCE/product.sha256"
stat -c '%n %s bytes mtime=%y' "$libdir/libcangjie-runtime.so" "$libdir/libboundscheck.so" \
    >"$EVIDENCE/product.stat"
sed -n -E '/^(CJ_SDK_VERSION|DISABLE_VERSION_CHECK|MRT_GC_UNIT_TESTS|MRT_TESTABLE_INTERNALS|CMAKE_BUILD_TYPE):/p' \
    "$BUILD/CMakeCache.txt" >"$EVIDENCE/configuration.cache"

GC_UNIT_OUT="$OUT" GCV2_RUNTIME_LIB_DIR="$libdir" MRT_TESTABLE_INTERNALS=1 \
    GC_UNIT_FILTER=M0Exit.RootFixRuntimeEnumerationClassifiesNoCopyAsS0 \
    taskset -c "$CORES" bash "$REPO/runtime/tests/gc_unit/run_standalone.sh" \
    >"$EVIDENCE/standalone-build-and-probe.log" 2>&1
standalone_rc=$?
printf '%s\n' "$standalone_rc" >"$EVIDENCE/standalone-build-and-probe.rc"
if [[ ! -x "$OUT/cj_gc_unit" ]]; then
    exit "$standalone_rc"
fi

sha256sum "$OUT/cj_gc_unit" >"$EVIDENCE/test-elf.sha256"
ldd "$OUT/cj_gc_unit" >"$EVIDENCE/test-elf.ldd"

tests=(
    M0Exit.RootFixRuntimeEnumerationClassifiesNoCopyAsS0
    M0Exit.RootFixClassifiesActiveOnlyUnusableCopyAsS1
    M0Exit.RootFixClassifiesRetiredOnlyUnusableCopyAsS1
    SegmentedArrayInit.EpochFlipRestartsAndRewritesPublishedBlock
    SegmentedArrayInit.EpochFlipRestartsAndRewritesPublishedBlockParallel
    SegmentedArrayInit.YoungGcRepairsIncompleteArrayRoot
    SegmentedArrayInit.YoungGcRepairsIncompleteArrayRootParallel
)

printf 'test\trc\n' >"$EVIDENCE/results.tsv"
for test_name in "${tests[@]}"; do
    safe_name=${test_name//./_}
    log="$EVIDENCE/${safe_name}.log"
    set +e
    LD_LIBRARY_PATH="$libdir" taskset -c "$CORES" \
        "$OUT/cj_gc_unit" "--gtest_filter=$test_name" >"$log" 2>&1
    test_rc=$?
    set -e
    printf '%s\n' "$test_rc" >"$EVIDENCE/${safe_name}.rc"
    printf '%s\t%s\n' "$test_name" "$test_rc" >>"$EVIDENCE/results.tsv"
done

{
    date -Ins
    uptime
    taskset -pc $$
    git -C "$REPO" status --short --branch
} >"$EVIDENCE/postflight.log" 2>&1
touch "$EVIDENCE/BASELINE_DONE"
