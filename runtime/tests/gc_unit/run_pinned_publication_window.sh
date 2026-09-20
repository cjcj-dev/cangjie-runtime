#!/usr/bin/env bash
# Build a fixture against the existing product SO, then inject a real GC with
# an external debugger. No product-source or product-build changes are made.
set -euo pipefail
ulimit -c 0
src=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$src/../../.." && pwd)
: "${GCV2_RUNTIME_LIB_DIR:?existing product SO directory required}"
: "${GC_UNIT_OUT:?independent output directory required}"
mkdir -p "$GC_UNIT_OUT"
headers=$(python3 "$root/runtime/build/resolve_runtime_headers.py" "$root/runtime" "$GCV2_RUNTIME_LIB_DIR")
# Keep inline readers' class layout identical to the linked product shape.
nm -D --defined-only "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" > "$GC_UNIT_OUT/product-exports.txt"
defines=()
if /usr/bin/grep -q PendingStalledAllocations "$GC_UNIT_OUT/product-exports.txt"; then
  defines=(-DMRT_GC_UNIT_TESTS=1 -DMRT_TESTABLE_INTERNALS=1 -DMRT_PRODUCT_TESTABLE_INTERNALS=1)
fi
"${CXX:-clang++}" "${defines[@]}" -std=gnu++17 -O0 -g -pthread -fno-rtti -fvisibility-inlines-hidden \
  -I"$src" -I"$root/runtime/src" -I"$root/runtime/src/Heap" \
  -I"$root/runtime/src/Heap/z/os/linux" -I"$root/runtime/include" -I"$headers/include" \
  -I"$root/runtime/src/CJThread/src/runtime/schedule/include" \
  -I"$root/runtime/third_party/third_party_bounds_checking_function/include" \
  "$src/pinned_publication_window.cpp" -L"$GCV2_RUNTIME_LIB_DIR" \
  -Wl,-rpath,"$GCV2_RUNTIME_LIB_DIR" -Wl,--exclude-libs,ALL -lcangjie-runtime -lboundscheck \
  -o "$GC_UNIT_OUT/pinned_publication_window"
sha256sum "$GC_UNIT_OUT/pinned_publication_window" "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" \
  "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" > "$GC_UNIT_OUT/identity.sha256"
nm --defined-only "$GC_UNIT_OUT/pinned_publication_window" | c++filt > "$GC_UNIT_OUT/fixture-defined.txt"
nm --defined-only "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" | c++filt > "$GC_UNIT_OUT/product-defined.txt"
uptime > "$GC_UNIT_OUT/uptime-before.txt"
set +e
LD_LIBRARY_PATH="$GCV2_RUNTIME_LIB_DIR" timeout 90s gdb -q -batch \
  -x "$src/pinned_publication_window.gdb" "$GC_UNIT_OUT/pinned_publication_window" \
  > "$GC_UNIT_OUT/gdb.log" 2>&1
rc=$?
printf '%s\n' "$rc" > "$GC_UNIT_OUT/gdb.rc"
uptime > "$GC_UNIT_OUT/uptime-after.txt"
exit "$rc"
