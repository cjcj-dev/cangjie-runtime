#!/usr/bin/env bash
set -uo pipefail

lane_root=/root/cj_build/impl_relocation_receipt_queue5-mutant
lane_runtime=$lane_root/src/runtime
lane_build=$lane_runtime/ImplBuildDefault
lane_lib=$lane_runtime/output/temp/lib/x86_64_Release
lane_test_out=$lane_runtime/tests/gc_unit/build_queue5_default

echo "PHASE=default-green"
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

if [[ $configure_rc -eq 0 ]]; then
    cmake --build "$lane_build" --target cangjie-runtime -j8
    build_rc=$?
else
    build_rc=125
fi
echo "PRODUCT_BUILD_RC=$build_rc"

if [[ -f "$lane_lib/libcangjie-runtime.so" ]]; then
    sha256sum "$lane_lib/libcangjie-runtime.so"
    nm -D -C "$lane_lib/libcangjie-runtime.so" | \
        /usr/bin/grep 'ForwardTask<.*>::Execute' > /tmp/queue5-default-forwardtask.nm || true
    echo "DEFAULT_FORWARD_TASK_EXPORTS_BEGIN"
    sed -n '1,20p' /tmp/queue5-default-forwardtask.nm
    echo "DEFAULT_FORWARD_TASK_EXPORTS_END"
    echo "DEFAULT_FORWARD_TASK_EXPORT_COUNT=$(/usr/bin/grep -c . /tmp/queue5-default-forwardtask.nm || true)"
else
    echo "PRODUCT_SO_MISSING"
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

if [[ $configure_rc -ne 0 || $build_rc -ne 0 || $test_rc -ne 0 ]]; then
    echo "FINAL_RC=1"
    exit 1
fi
echo "FINAL_RC=0"
