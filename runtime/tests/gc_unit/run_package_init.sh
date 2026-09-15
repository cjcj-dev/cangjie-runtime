#!/usr/bin/env bash
# Focused ABI fixture linked to the built product SO; supports the same ELF
# against original, fault-injected and restored products.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
SRC="$ROOT/runtime/tests/gc_unit"
: "${GCV2_RUNTIME_LIB_DIR:?product SO directory required}"
: "${GC_UNIT_OUT:?evidence directory required}"
mkdir -p "$GC_UNIT_OUT"
OUT="$GC_UNIT_OUT"
LIB="$GCV2_RUNTIME_LIB_DIR"
HEADERS="${GCV2_RUNTIME_OUTPUT_ROOT:-$(realpath -m "$LIB/../..")}/include"
ELF="${PACKAGE_INIT_ELF:-$OUT/package_init_unit}"
ulimit -c 0
uptime > "$OUT/uptime-before.txt"
df -h /root > "$OUT/disk-before.txt"
start=$SECONDS
flags=()
if [[ "${MRT_TESTABLE_INTERNALS:-0}" == 1 ]]; then flags=(-DMRT_TESTABLE_INTERNALS=1 -DMRT_GC_UNIT_TESTS=1); fi
if [[ -z "${PACKAGE_INIT_ELF:-}" ]]; then
    objects=()
    pids=()
    clang++ -std=gnu++17 -O0 -g -pthread -fno-rtti -fPIC -shared \
        -I"$ROOT/runtime/src" -I"$ROOT/runtime/src/Heap" -I"$ROOT/runtime/include" \
        -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include" \
        -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include" -I"$HEADERS" \
        "$SRC/package_init_image.cpp" -o "$OUT/libcj_package_init_fixture.so" > "$OUT/plugin.build.log" 2>&1 &
    pids+=("$!")
    for source in gc_unit_main.cpp gc_cycle_sequence_fixture.cpp test_package_init.cpp; do
        object="$OUT/$source.o"
        objects+=("$object")
        clang++ -std=gnu++17 -O0 -g -pthread -fno-rtti -fexceptions -fvisibility-inlines-hidden \
            -I"$SRC" -I"$ROOT/runtime/src/Loader/BinaryFile" -I"$ROOT/runtime/src" -I"$ROOT/runtime/src/Heap" -I"$ROOT/runtime/include" \
            -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include" \
            -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include" -I"$HEADERS" \
            "${flags[@]}" -c "$SRC/$source" -o "$object" > "$OUT/$source.build.log" 2>&1 &
        pids+=("$!")
    done
    build_rc=0
    for pid in "${pids[@]}"; do wait "$pid" || build_rc=$?; done
    if [[ $build_rc -eq 0 ]]; then
        clang++ -pthread "${objects[@]}" -L"$LIB" -Wl,-rpath,"$LIB" -Wl,--exclude-libs,ALL \
            -lcangjie-runtime -lboundscheck -ldl -o "$ELF" > "$OUT/link.log" 2>&1
        build_rc=$?
    fi
    echo "$build_rc" > "$OUT/build.rc"
    echo "build wall=$((SECONDS-start)) rc=$build_rc parallel_tus=4"
    if [[ $build_rc -ne 0 ]]; then
        grep -n -m 12 -E 'error:|undefined reference' "$OUT"/*.log
        exit "$build_rc"
    fi
fi
sha256sum "$(dirname "$ELF")/libcj_package_init_fixture.so" "$ELF" "$LIB/libcangjie-runtime.so" "$LIB/libboundscheck.so" > "$OUT/lineage.sha256"
nm --defined-only "$LIB/libcangjie-runtime.so" > "$OUT/product.defined.txt"
nm --defined-only "$ELF" > "$OUT/fixture.defined.txt"
start=$SECONDS
filter=()
if [[ -n "${PACKAGE_INIT_FILTER:-}" ]]; then filter=("--gtest_filter=$PACKAGE_INIT_FILTER"); fi
LD_LIBRARY_PATH="$LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    taskset -c "${PACKAGE_INIT_CORES:-0-15}" timeout 60 "$ELF" \
    "${filter[@]}" > "$OUT/run.log" 2>&1
rc=$?
echo "$rc" > "$OUT/run.rc"
uptime > "$OUT/uptime-after.txt"
echo "run wall=$((SECONDS-start)) rc=$rc cores=${PACKAGE_INIT_CORES:-0-15} elf=$ELF"
grep -E 'PACKAGE_INIT_TARGET|FAIL|tests:|INCOMPLETE' "$OUT/run.log" | tail -65
exit "$rc"
