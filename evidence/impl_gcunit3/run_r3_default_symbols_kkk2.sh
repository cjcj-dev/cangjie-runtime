#!/usr/bin/env bash
set -uo pipefail

readonly REPO=/root/impl_gcunit3-aa48-run
readonly BUILD="$REPO/runtime/CMakebuild_gcunit3_dual_off"
readonly LIVE_LIB="$REPO/runtime/output/temp/lib/x86_64_Release"
readonly BASELINE=/root/rev_gcunit3-default-symbol/evidence/baseline.r4.symbols
readonly ON_SO=/root/sodepot/impl_gcunit3/r3-green/libcangjie-runtime.so
readonly OUT="$REPO/evidence/impl_gcunit3/r3-default-symbols"
readonly DEPOT=/root/sodepot/impl_gcunit3/r3-default-off
readonly CORES=16-31

mkdir -p "$OUT" "$DEPOT"
{
    date -Ins
    uptime
    taskset -pc $$
    echo 'RECIPE=cmake --build CMakebuild_gcunit3_dual_off --target cangjie-runtime --parallel 16'
    sed -n -E '/^(CJ_SDK_VERSION|DISABLE_VERSION_CHECK|MRT_GC_UNIT_TESTS|MRT_TESTABLE_INTERNALS|CMAKE_BUILD_TYPE):/p' \
        "$BUILD/CMakeCache.txt"
    sha256sum "$BASELINE"
} >"$OUT/preflight.log" 2>&1

cd "$REPO/runtime" || exit $?
GC_UNIT_GATE_SKIP=1 taskset -c "$CORES" cmake --build "$BUILD" \
    --target cangjie-runtime --parallel 16 >"$OUT/build.log" 2>&1
build_rc=$?
printf '%s\n' "$build_rc" >"$OUT/build.rc"
if [[ $build_rc -ne 0 ]]; then
    exit "$build_rc"
fi
cp "$LIVE_LIB/libcangjie-runtime.so" "$DEPOT/"
cp "$LIVE_LIB/libboundscheck.so" "$DEPOT/"
sha256sum "$DEPOT/libcangjie-runtime.so" "$DEPOT/libboundscheck.so" >"$OUT/products.sha256"
stat -c '%y %n' "$DEPOT/libcangjie-runtime.so" "$DEPOT/libboundscheck.so" >"$OUT/products.stat"

nm --defined-only "$DEPOT/libcangjie-runtime.so" | awk '{print $NF}' | sort -u >"$OUT/delivery.symbols"
nm --defined-only "$DEPOT/libcangjie-runtime.so" | awk '{print $NF}' | sort -u >"$OUT/delivery.self.symbols"
diff -u "$BASELINE" "$OUT/delivery.symbols" >"$OUT/baseline-delivery.diff"
printf '%s\n' "$?" >"$OUT/baseline-delivery.diff.rc"
diff -u "$OUT/delivery.symbols" "$OUT/delivery.self.symbols" >"$OUT/self-noise.diff"
printf '%s\n' "$?" >"$OUT/self-noise.diff.rc"

nm --defined-only "$ON_SO" | /usr/bin/grep 'CJ_MRT_SetLargeArrayInitTestHooks' >"$OUT/hook-on.txt"
printf '%s\n' "${PIPESTATUS[1]}" >"$OUT/hook-on.rc"
nm --defined-only "$DEPOT/libcangjie-runtime.so" | \
    /usr/bin/grep 'CJ_MRT_SetLargeArrayInitTestHooks' >"$OUT/hook-off.txt"
printf '%s\n' "${PIPESTATUS[1]}" >"$OUT/hook-off.rc"
{
    date -Ins
    uptime
    wc -l "$BASELINE" "$OUT/delivery.symbols" "$OUT/baseline-delivery.diff"
} >"$OUT/postflight.log" 2>&1
touch "$OUT/R3_DEFAULT_SYMBOLS_DONE"
