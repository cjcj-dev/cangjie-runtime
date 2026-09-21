#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
src=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$src/../../.." && pwd)
: "${GCV2_RUNTIME_LIB_DIR:?product SO directory required}"
: "${GC_UNIT_OUT:?independent output directory required}"
mkdir -p "$GC_UNIT_OUT"
headers=$(python3 "$root/runtime/build/resolve_runtime_headers.py" "$root/runtime" "$GCV2_RUNTIME_LIB_DIR")
"${CXX:-clang++}" -std=gnu++17 -O0 -g -pthread -fno-rtti -fvisibility-inlines-hidden \
  -I"$src" -I"$root/runtime/src" -I"$root/runtime/src/Heap" \
  -I"$root/runtime/src/Heap/z/os/linux" -I"$root/runtime/include" -I"$headers/include" \
  -I"$root/runtime/src/CJThread/src/runtime/schedule/include" \
  -I"$root/runtime/third_party/third_party_bounds_checking_function/include" \
  "$src/pinned_allocation_reclaim.cpp" -L"$GCV2_RUNTIME_LIB_DIR" \
  -Wl,-rpath,"$GCV2_RUNTIME_LIB_DIR" -Wl,--exclude-libs,ALL -lcangjie-runtime -lboundscheck \
  -o "$GC_UNIT_OUT/pinned_allocation_reclaim"
sha256sum "$GC_UNIT_OUT/pinned_allocation_reclaim" "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" \
  "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" > "$GC_UNIT_OUT/identity.sha256"
for mode in sparse empty; do
  (
    uptime > "$GC_UNIT_OUT/$mode-uptime-before.txt"
    set +e
    LD_LIBRARY_PATH="$GCV2_RUNTIME_LIB_DIR" timeout 90 "$GC_UNIT_OUT/pinned_allocation_reclaim" "$mode" \
      > "$GC_UNIT_OUT/$mode.log" 2>&1
    echo "$?" > "$GC_UNIT_OUT/$mode.rc"
    uptime > "$GC_UNIT_OUT/$mode-uptime-after.txt"
  ) &
done
wait
cat "$GC_UNIT_OUT/sparse.rc" "$GC_UNIT_OUT/empty.rc"
[[ $(cat "$GC_UNIT_OUT/sparse.rc") = 0 && $(cat "$GC_UNIT_OUT/empty.rc") = 0 ]]
