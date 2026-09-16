#!/bin/bash
# Invoke inside wf_kkk2.sh sh (shared test slot); use cjops windows allocation.
set -u
ulimit -c 0
root=$1
config=$2
cores=$3
label=${4:-green}
elfroot=${5:-$root/unit-$config}
lib=${6:-$root/$config/build/runtime-staging/lib/x86_64_Release}
src=$root/$config/runtime/tests/gc_unit
out=$root/$label-$config
mkdir -p "$out"
export GC_UNIT_JOBS=192 GC_UNIT_BUILD_JOBS=192
export GCV2_RUNTIME_LIB_DIR=$lib GCV2_RUNTIME_OUTPUT_ROOT=$root/$config/build/runtime-staging
export MRT_TESTABLE_INTERNALS=0
[ "$config" = testable ] && export MRT_TESTABLE_INTERNALS=1
uptime > "$out/uptime-before.txt"
start=$SECONDS
if [ ! -f "$elfroot/cj_gc_unit" ]; then
    GC_UNIT_OUT=$elfroot taskset -c "$cores" bash "$src/run_standalone.sh" > "$out/compile-run.log" 2>&1
    echo "$?" > "$out/compile-run.rc"
fi
if [ ! -f "$elfroot/cj_gc_unit" ] || [ ! -f "$elfroot/cj_gc_forwarding_publication_unit" ]; then
    echo "P07_ELF_UNAVAILABLE config=$config"
    /usr/bin/grep -m 8 'error:' "$out/compile-run.log"
    exit 2
fi
sha256sum "$elfroot/cj_gc_unit" "$elfroot/cj_gc_forwarding_publication_unit" > "$out/elf.sha256"
sha256sum "$lib"/*.so > "$out/so.sha256"
count=3
[[ "$label" = cut-* ]] && count=1
for sample in $(seq 1 "$count"); do
    (
        run=$out/run-$sample
        mkdir -p "$run"
        uptime > "$run/uptime-before.txt"
        env LD_LIBRARY_PATH="$lib" taskset -c "$cores" bash "$src/run_parallel_tests.sh" \
            "$elfroot/cj_gc_unit" "$elfroot/cj_gc_forwarding_publication_unit" "$run" "$lib" > "$run/run.log" 2>&1
        rc=$?; echo "$rc" > "$run/run.rc"
        uptime > "$run/uptime-after.txt"
        echo "P07_UNIT config=$config label=$label sample=$sample rc=$rc"
        /usr/bin/grep -E '^\[========\] [0-9]{2,} tests:|^\[  FAILED  \] [A-Za-z]|^GC_UNIT_INCOMPLETE' "$run/run.log" | tail -30
    ) &
done
wait
filters='ZWorkers.CoordinatorReturnsAfterEveryWorkerCompleted ZWorkers.RunGivesEachActiveWorkerOneDistinctIdBelowActive ZWorkers.RunAccumulatesParallelTimeInStatWorkers GcDirector.WorkerStatsIncludeInFlightBatch MarkingSMR.WorkerPopRetiresInCurrentSlot NativeRootCurrent.MajorSeed NativeRootCurrent.StrongFinalizerRootPublishesAndMarks'
for filter in $filters; do
    env LD_LIBRARY_PATH="$lib" taskset -c "$cores" timeout 60s "$elfroot/cj_gc_unit" --gtest_filter="$filter" > "$out/filter-$filter.log" 2>&1
    echo "$?" > "$out/filter-$filter.rc"
done
nm -C --defined-only "$lib/libcangjie-runtime.so" > "$out/product.nm"
nm -C --defined-only "$elfroot/cj_gc_unit" > "$out/test.nm"
echo "wall=$((SECONDS-start))" > "$out/wall.txt"
uptime > "$out/uptime-after.txt"
