#!/usr/bin/env bash
# Focused executable uses the same registry and product configuration as gc_unit.
set -euo pipefail
ulimit -c 0
root=$(cd "$(dirname "$0")/../../.." && pwd)
src=$root/runtime/tests/gc_unit
out=${GC_UNIT_OUT:?}
rt=${GCV2_RUNTIME_LIB_DIR:?}
mkdir -p "$out"
uptime > "$out/uptime-before.txt"
start=$SECONDS
if [[ ${GC_TOTALS_REUSE_ELF:-0} != 1 ]]; then
    config=$(python3 "$src/product_test_configuration.py" "$root/runtime" "$rt" "${GCV2_RUNTIME_OUTPUT_ROOT:-$rt/../..}")
    read -r testable unit ohos <<< "$config"
    flags=(-std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti -fvisibility-inlines-hidden)
    [[ $testable != 1 ]] || flags+=(-DMRT_TESTABLE_INTERNALS=1 -DMRT_PRODUCT_TESTABLE_INTERNALS=1)
    [[ $unit != 1 ]] || flags+=(-DMRT_GC_UNIT_TESTS=1)
    for path in tests/gc_unit src src/Loader/BinaryFile src/Heap src/Heap/z/os/linux src/CJThread/src/runtime/schedule/include include third_party/third_party_bounds_checking_function/include; do
        flags+=(-I"$root/runtime/$path")
    done
    flags+=(-I"${GCV2_RUNTIME_OUTPUT_ROOT:-$rt/../..}/include")
    pids=()
    for file in gc_unit_main test_gc_totals; do
        "${CXX:-clang++}" "${flags[@]}" -c "$src/$file.cpp" -o "$out/$file.o" > "$out/$file-build.log" 2>&1 &
        pids+=("$!")
    done
    for pid in "${pids[@]}"; do wait "$pid"; done
    "${CXX:-clang++}" "${flags[@]}" "$out/gc_unit_main.o" "$out/test_gc_totals.o" \
        -L"$rt" -Wl,--exclude-libs,ALL -lcangjie-runtime -lboundscheck -o "$out/gc_totals_native" > "$out/link.log" 2>&1
    sha256sum "$out/gc_totals_native" > "$out/elf.sha256"
fi
sha256sum "$out/gc_totals_native" "$rt/libcangjie-runtime.so" "$rt/libboundscheck.so" > "$out/artifacts.sha256"
LD_LIBRARY_PATH="$rt" ldd "$out/gc_totals_native" > "$out/ldd.txt"
# Fresh processes isolate each collector and metric. Independent arms run concurrently.
pids=()
for test in MinorTime MinorFreed MajorTime MajorFreed; do
    (
        set +e
        LD_LIBRARY_PATH="$rt" timeout 60s "$out/gc_totals_native" "--gtest_filter=GCTotals1322.$test" > "$out/$test.log" 2>&1
        echo "$?" > "$out/$test.rc"
    ) &
    pids+=("$!")
done
for pid in "${pids[@]}"; do wait "$pid"; done
rc=0
for test in MinorTime MinorFreed MajorTime MajorFreed; do
    result=$(cat "$out/$test.rc")
    echo "$test rc=$result"
    [[ $result == 0 ]] || rc=1
done
echo "$rc" > "$out/run.rc"
printf 'wall=%s\n' "$((SECONDS-start))" > "$out/wall.txt"
uptime > "$out/uptime-after.txt"
exit "$rc"
