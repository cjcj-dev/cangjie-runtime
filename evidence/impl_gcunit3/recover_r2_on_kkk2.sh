#!/usr/bin/env bash
set -uo pipefail

readonly REPO=/root/impl_gcunit3-aa48-run
readonly BUILD="$REPO/runtime/CMakebuild_gcunit3_recover_on"
readonly OUT="$REPO/evidence/impl_gcunit3/r2-recover-on"
readonly CORES=16-31

mkdir -p "$OUT"
rm -f "$OUT/R2_RECOVER_ON_DONE"
{
    date -Ins
    uptime
    taskset -pc $$
    git -C "$REPO" status --short --untracked-files=no
} >"$OUT/preflight.log" 2>&1

taskset -c "$CORES" cmake -S "$REPO/runtime" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCJ_SDK_VERSION=0.0.1 \
    -DDISABLE_VERSION_CHECK=ON \
    -DMRT_GC_UNIT_TESTS=ON \
    -DMRT_TESTABLE_INTERNALS=ON \
    >"$OUT/configure.log" 2>&1
configure_rc=$?
printf '%s\n' "$configure_rc" >"$OUT/configure.rc"
if [[ $configure_rc -ne 0 ]]; then
    exit "$configure_rc"
fi

GC_UNIT_GATE_SKIP=1 taskset -c "$CORES" cmake --build "$BUILD" \
    --target cangjie-runtime --parallel 16 >"$OUT/build.log" 2>&1
build_rc=$?
printf '%s\n' "$build_rc" >"$OUT/build.rc"
if [[ $build_rc -ne 0 ]]; then
    exit "$build_rc"
fi

sha256sum "$REPO/runtime/output/temp/lib/x86_64_Release/libcangjie-runtime.so" \
    "$REPO/runtime/output/temp/lib/x86_64_Release/libboundscheck.so" \
    >"$OUT/products.sha256"
{
    date -Ins
    uptime
    taskset -pc $$
} >"$OUT/postflight.log" 2>&1
touch "$OUT/R2_RECOVER_ON_DONE"
