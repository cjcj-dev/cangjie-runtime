#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
source_root=$(cd "$(dirname "$0")/../.." && pwd)
fixture_root="$source_root/tests/gc_unit"
product_root=${GCV2_RUNTIME_LIB_DIR:?}
output_root=${SCHEDULER_OUT:?}
mkdir -p "$output_root"
include_flags=(-I"$source_root/src" -I"$source_root/include" -I"$source_root/src/Heap/z/os/linux"
    -I"$source_root/third_party/third_party_bounds_checking_function/include")
while IFS= read -r include_dir; do
    include_flags+=(-I"$include_dir")
done < <(find "$source_root/src/CJThread/src" -type d -name include | sort)
include_flags+=(-I"$source_root/src/CJThread/src/runtime/netpoll/include/inner")
include_flags+=(-I"$source_root/src/CJThread/src/runtime/schedule/include/inner/gas/x86/x86_64")
include_flags+=(-I"$source_root/src/CJThread/src/runtime/netpoll/include/linux/inner")
clang++ -std=c++17 -g -O0 -pthread -DMRT_USE_CJTHREAD_RENAME \
    -include "$source_root/src/CJThread/src/base/mid/include/schedule_rename.h" \
    -ffunction-sections -Wl,--gc-sections "${include_flags[@]}" \
    "$fixture_root/scheduler_queue_layout.cpp" -L"$product_root" -Wl,-rpath,"$product_root" \
    -lcangjie-runtime -lboundscheck -o "$output_root/layout"
"$output_root/layout" > "$output_root/layout.json"
clang++ -std=c++17 -g -O0 -pthread -I"$source_root/src" \
    "$fixture_root/scheduler_queue_fixture.cpp" -L"$product_root" \
    -Wl,-rpath,"$product_root" -lcangjie-runtime -lboundscheck -o "$output_root/fixture"
sha256sum "$output_root/fixture" "$product_root/libcangjie-runtime.so" \
    "$product_root/libboundscheck.so" > "$output_root/identity.sha256"
nm --defined-only "$product_root/libcangjie-runtime.so" > "$output_root/product.nm"
nm --defined-only "$output_root/fixture" > "$output_root/fixture.nm"
export LD_LIBRARY_PATH="$product_root${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SCHEDULER_PRODUCT="$product_root/libcangjie-runtime.so"
export SCHEDULER_LAYOUT="$output_root/layout.json"
timeout 90 gdb -q -nx -batch -x "$fixture_root/test_scheduler_queue_gdb.py" "$output_root/fixture"
