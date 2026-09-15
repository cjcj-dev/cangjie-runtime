#!/bin/bash
# Run inside wf_kkk2.sh sh; isolated SATB companion from the cycle runner.
set -u
ulimit -c 0
root=$1
out=$2
lib=$3
cores=$4
elf=${5:-$out/generation_satb_obligations}
src=$root/runtime/tests/gc_unit
mkdir -p "$out"
uptime > "$out/uptime-before.txt"
start=$SECONDS
if [ ! -f "$elf" ]; then
    headers=$(python3 "$root/runtime/build/resolve_runtime_headers.py" "$root/runtime" "$lib") || exit $?
    clang++ -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti \
        -fvisibility-inlines-hidden -DMRT_TESTABLE_INTERNALS=1 \
        -DMRT_PRODUCT_TESTABLE_INTERNALS=1 -DMRT_GC_UNIT_TESTS=1 \
        -I"$src" -I"$root/runtime/src" -I"$root/runtime/src/Heap" \
        -I"$root/runtime/src/CJThread/src/runtime/schedule/include" \
        -I"$root/runtime/include" -I"$headers/include" \
        -I"$root/runtime/third_party/third_party_bounds_checking_function/include" \
        "$src/gc_unit_main.cpp" "$src/gc_cycle_sequence_fixture.cpp" "$src/gc_worker_fixture.cpp" \
        "$src/test_generation_satb_obligations.cpp" \
        -L"$lib" -Wl,-rpath,"$lib" -Wl,--exclude-libs,ALL \
        -lcangjie-runtime -lboundscheck -ldl -o "$elf" > "$out/compile.log" 2>&1
    rc=$?; echo "$rc" > "$out/compile.rc"
    [ "$rc" = 0 ] || { head -35 "$out/compile.log"; exit "$rc"; }
fi
sha256sum "$elf" > "$out/elf.sha256"
sha256sum "$lib"/*.so > "$out/so.sha256"
LD_LIBRARY_PATH="$lib" ldd "$elf" > "$out/ldd.txt"
nm -C --defined-only "$lib/libcangjie-runtime.so" > "$out/product.nm"
for sample in 1 2 3; do
    (
        run=$out/run-$sample
        mkdir -p "$run"
        failed=0
        for name in YoungMarkWorkDoesNotConsumeOldStripes MarkCompleteStopsOldPublication \
            BlockedWeakReadSeparatesOldStrongAndFinalizable BlockedWeakReadKeepsYoungAlive \
            UnblockedWeakReadPublishesOldKeepAlive; do
            env LD_LIBRARY_PATH="$lib" taskset -c "$cores" timeout 60s "$elf" \
                --gtest_filter="GenerationMark.$name" > "$run/$name.log" 2>&1
            rc=$?; echo "$rc" > "$run/$name.rc"
            [ "$rc" = 0 ] || failed=$((failed+1))
            echo "SATB_RESULT sample=$sample test=$name rc=$rc"
            /usr/bin/grep -E 'SATB_INPUT|SATB_TARGET|EXPECT|tests:' "$run/$name.log"
        done
        echo "$failed" > "$run/run.rc"
    ) &
done
wait
echo "wall=$((SECONDS-start))" > "$out/wall.txt"
uptime > "$out/uptime-after.txt"
