#!/usr/bin/env bash
set -uo pipefail

lane_root=/root/cj_build/impl_relocation_receipt_queue4-default
lane_runtime=$lane_root/src/runtime
lane_build=$lane_runtime/ImplBuildDefault
lane_lib=$lane_runtime/output/temp/lib/x86_64_Release
lane_test_out=$lane_runtime/tests/gc_unit/build_queue4_default

echo "PHASE=default-product-green"
date -Is
uptime
cmake -S "$lane_runtime" -B "$lane_build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DMRT_TESTABLE_INTERNALS=OFF \
    -DMRT_GC_UNIT_TESTS=OFF
configure_rc=$?
echo "CONFIGURE_RC=$configure_rc"
if [[ -f "$lane_build/CMakeCache.txt" ]]; then
    /usr/bin/grep -E '^MRT_(GC_UNIT_TESTS|TESTABLE_INTERNALS):' "$lane_build/CMakeCache.txt" || true
fi

if [[ $configure_rc -eq 0 ]]; then
    cmake --build "$lane_build" --target cangjie-runtime -j8
    build_rc=$?
else
    build_rc=125
fi
echo "PRODUCT_BUILD_RC=$build_rc"

if [[ -f "$lane_lib/libcangjie-runtime.so" ]]; then
    sha256sum "$lane_lib/libcangjie-runtime.so"
    echo "DEFAULT_FORWARD_TASK_DYNAMIC_EXPORTS_BEGIN"
    nm -D -C "$lane_lib/libcangjie-runtime.so" | /usr/bin/grep 'ForwardTask<.*>::Execute' || true
    export_count=$(nm -D -C "$lane_lib/libcangjie-runtime.so" |
        /usr/bin/grep -c 'ForwardTask<.*>::Execute' || true)
    echo "DEFAULT_FORWARD_TASK_DYNAMIC_EXPORTS=$export_count"
    echo "DEFAULT_MUTATOR_TEST_ACCESS_SYMBOLS=$(nm -D -C "$lane_lib/libcangjie-runtime.so" |
        /usr/bin/grep -c 'MutatorPublishTestAccess' || true)"
    echo "DEFAULT_FORWARD_TASK_DYNAMIC_EXPORTS_END"
else
    echo "PRODUCT_SO_MISSING"
    export_count=125
fi

if [[ $build_rc -eq 0 ]]; then
    GCV2_RUNTIME_LIB_DIR="$lane_lib" GC_UNIT_OUT="$lane_test_out" MRT_TESTABLE_INTERNALS=0 \
        bash "$lane_runtime/tests/gc_unit/run_standalone.sh"
    test_rc=$?
else
    test_rc=125
fi
echo "TEST_RC=$test_rc"
date -Is
uptime

if [[ $configure_rc -ne 0 || $build_rc -ne 0 || $test_rc -ne 0 || $export_count -ne 0 ]]; then
    echo "FINAL_RC=1"
    exit 1
fi
echo "FINAL_RC=0"
