#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
OUT=${GC_UNIT_OUT:?set GC_UNIT_OUT}
SDK=${CANGJIE_HOME:?set CANGJIE_HOME to a jointly rebuilt SDK}
RT=${GCV2_RUNTIME_LIB_DIR:?set GCV2_RUNTIME_LIB_DIR}
HOST=${GC_UNIT_CJC_RUNTIME_LIB_DIR:?set the compiler host runtime}
CJC=${CJC:-$SDK/bin/cjc}
mkdir -p "$OUT" "$OUT/temps"
uptime > "$OUT/uptime-before.txt"
start=$SECONDS
${CXX:-clang++} -std=c++17 -shared -fPIC -O0 -I"$ROOT/runtime/src" -I"$ROOT/runtime/include" \
  -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include" -I"$RT/../../include" \
  -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include" \
  "$ROOT/runtime/tests/gc_unit/tlab_inline_observe.cpp" -L"$RT" -lcangjie-runtime -lboundscheck \
  -o "$OUT/libtlab_inline_observe.so" > "$OUT/native-build.log" 2>&1
LD_LIBRARY_PATH="$HOST:$SDK/third_party/llvm/lib:$SDK/tools/lib" \
  "$CJC" "$ROOT/runtime/tests/gc_unit/tlab_inline_managed.cj" -O0 --static-std --save-temps "$OUT/temps" \
  -L"$OUT" -ltlab_inline_observe -o "$OUT/tlab_inline_managed" > "$OUT/managed-build.log" 2>&1
sha256sum "$OUT/tlab_inline_managed" "$OUT/libtlab_inline_observe.so" \
  "$RT/libcangjie-runtime.so" "$RT/libboundscheck.so" "$SDK/third_party/llvm/bin/llc" \
  "$SDK/third_party/llvm/bin/opt" > "$OUT/inputs.sha256"
for mode in fast refill; do
  (
    set +e
    LD_LIBRARY_PATH="$OUT:$RT:$SDK/runtime/lib/linux_x86_64_cjnative" \
      cjGCInterval=3600s timeout 60 "$OUT/tlab_inline_managed" "$mode" > "$OUT/$mode.log" 2>&1
    echo "$?" > "$OUT/$mode.rc"
  ) &
done
wait
uptime > "$OUT/uptime-after.txt"
echo "wall=$((SECONDS-start)) jobs=${CANGJIE_BUILD_JOBS:-unset} arms=2" > "$OUT/wall.txt"
rc=0
for mode in fast refill; do
  result=$(cat "$OUT/$mode.rc")
  echo "TLAB_MANAGED mode=$mode rc=$result"
  [ "$result" = 0 ] || rc=1
done
exit "$rc"
