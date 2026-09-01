#!/usr/bin/env bash
set -uo pipefail

readonly REPO=/root/impl_gcunit3-aa48-run
readonly BUILD="$REPO/runtime/CMakebuild_gcunit3"
readonly LIVE_LIB="$REPO/runtime/output/temp/lib/x86_64_Release"
readonly GREEN=/root/sodepot/impl_gcunit3/r4-current-green
readonly OUT="$REPO/evidence/impl_gcunit3/r4-managed-product-arms"
readonly BACKUP=/root/impl_gcunit3-r4-managed-backup.cpp
readonly MUTATOR="$REPO/runtime/src/Mutator/Mutator.cpp"
readonly CORES=16-31

mkdir -p "$OUT"
cp "$MUTATOR" "$BACKUP"

restore_source()
{
    cp "$BACKUP" "$MUTATOR"
}
trap restore_source EXIT

run_modes()
{
    local libdir=$1
    local out=$2
    printf 'mode\trc\n' >"$out/results.tsv"
    for mode in full young; do
        mkdir -p "$out/managed-$mode"
        set +e
        CANGJIE_HOME=/root/rebuild_b_sdk CJC=/root/rebuild_b_sdk/bin/cjc \
            GCV2_RUNTIME_LIB_DIR="$libdir" GC_UNIT_OUT="$out/managed-$mode" \
            taskset -c "$CORES" bash "$REPO/runtime/tests/gc_unit/run_segmented_array_managed.sh" "$mode" \
            >"$out/managed-$mode.log" 2>&1
        local rc=$?
        set -e
        printf '%s\n' "$rc" >"$out/managed-$mode.rc"
        printf '%s\t%s\n' "$mode" "$rc" >>"$out/results.tsv"
    done
}

run_arm()
{
    local name=$1
    local patch=$2
    local out="$OUT/$name"
    local depot="/root/sodepot/impl_gcunit3/r4-$name"
    mkdir -p "$out" "$depot"
    restore_source
    git -C "$REPO" apply "$patch"
    printf '%s\n' "$?" >"$out/apply.rc"
    git -C "$REPO" diff -- runtime/src/Mutator/Mutator.cpp >"$out/cut.diff"
    {
        date -Ins
        uptime
        sha256sum "$MUTATOR" "$GREEN/libboundscheck.so"
    } >"$out/preflight.log"
    cd "$REPO/runtime" || return $?
    GC_UNIT_GATE_SKIP=1 taskset -c "$CORES" cmake --build "$BUILD" \
        --target cangjie-runtime --parallel 16 >"$out/build.log" 2>&1
    local build_rc=$?
    printf '%s\n' "$build_rc" >"$out/build.rc"
    if [[ $build_rc -ne 0 ]]; then
        return "$build_rc"
    fi
    cp "$LIVE_LIB/libcangjie-runtime.so" "$depot/"
    cp "$GREEN/libboundscheck.so" "$depot/"
    sha256sum "$depot/libcangjie-runtime.so" "$depot/libboundscheck.so" >"$out/products.sha256"
    stat -c '%y %n' "$depot/libcangjie-runtime.so" "$depot/libboundscheck.so" >"$out/products.stat"
    run_modes "$depot" "$out"
    { date -Ins; uptime; } >"$out/postflight.log"
}

sha256sum "$MUTATOR" >"$OUT/source-before.sha256"
run_arm cut-mutator-managed /root/r4-cut-mutator-managed.patch || exit $?
run_arm cut-watermark-managed /root/r4-cut-watermark-managed.patch || exit $?

restore_source
sha256sum "$MUTATOR" >"$OUT/source-restored.sha256"
cmp -s "$BACKUP" "$MUTATOR"
printf '%s\n' "$?" >"$OUT/source-restore.cmp.rc"

readonly RESTORED=/root/sodepot/impl_gcunit3/r4-managed-restored
readonly RESTORED_OUT="$OUT/restored"
mkdir -p "$RESTORED" "$RESTORED_OUT"
cd "$REPO/runtime" || exit $?
GC_UNIT_GATE_SKIP=1 taskset -c "$CORES" cmake --build "$BUILD" \
    --target cangjie-runtime --parallel 16 >"$RESTORED_OUT/build.log" 2>&1
restored_build_rc=$?
printf '%s\n' "$restored_build_rc" >"$RESTORED_OUT/build.rc"
if [[ $restored_build_rc -ne 0 ]]; then
    exit "$restored_build_rc"
fi
cp "$LIVE_LIB/libcangjie-runtime.so" "$RESTORED/"
cp "$GREEN/libboundscheck.so" "$RESTORED/"
sha256sum "$RESTORED/libcangjie-runtime.so" "$RESTORED/libboundscheck.so" >"$RESTORED_OUT/products.sha256"
stat -c '%y %n' "$RESTORED/libcangjie-runtime.so" "$RESTORED/libboundscheck.so" >"$RESTORED_OUT/products.stat"
run_modes "$RESTORED" "$RESTORED_OUT"
trap - EXIT
touch "$OUT/R4_MANAGED_PRODUCT_ARMS_DONE"
