#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit"
OUT="${GC_UNIT_OUT:?set GC_UNIT_OUT}"
LLC="${LLC:?set LLC to candidate llc}"
SO="${GCV2_RUNTIME_LIB_DIR:?}"
mkdir -p "$OUT"
"$LLC" --cangjie-pipeline -O0 -filetype=obj -o "$OUT/slot_domain_consumer.o" \
  "$SRC/slot_domain_consumer.ll"
clang -O0 -fPIC -shared -o "$OUT/libslot_mcc_stubs.so" "$SRC/slot_domain_mcc_stubs.c"
clang++ -std=gnu++17 -O0 -g -fno-rtti \
  -I"$ROOT/runtime/src" -I"$ROOT/runtime/src/Heap" -I"$ROOT/runtime/include" \
  -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include" \
  -I"$ROOT/runtime/src/CJThread/src/base/log/include" \
  -I"$ROOT/runtime/src/CJThread/src/runtime/log/include" \
  -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include" \
  "$SRC/slot_domain_driver.cpp" "$OUT/slot_domain_consumer.o" \
  -L"$SO" -Wl,-rpath,"$SO" -lcangjie-runtime -lboundscheck -ldl -lpthread \
  -o "$OUT/slot_domain_driver"
sha256sum "$LLC" "$SO/libcangjie-runtime.so" "$SO/libboundscheck.so" \
  "$OUT/slot_domain_driver" "$OUT/libslot_mcc_stubs.so" >"$OUT/ident.sha256"
set +e
LD_PRELOAD="$OUT/libslot_mcc_stubs.so" LD_LIBRARY_PATH="$SO${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  timeout 30s "$OUT/slot_domain_driver"
rc=$?
set -e
echo "SLOT_DOMAIN_DRIVER_RC=$rc"
exit "$rc"
