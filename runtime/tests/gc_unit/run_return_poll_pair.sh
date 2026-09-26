#!/usr/bin/env bash
# Usage: LLC=/paired/llc GCV2_RUNTIME_LIB_DIR=/product/lib OUT=/evidence bash "$0"
set -euo pipefail
ulimit -c 0
src=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$src/../../.." && pwd)
: "${LLC:?paired LLVM compiler required}" "${GCV2_RUNTIME_LIB_DIR:?product runtime required}" "${OUT:?evidence directory required}"
[[ $(uname -m) == x86_64 && $(uname -s) == Linux ]]
mkdir -p "$OUT"
headers=${GCV2_RUNTIME_OUTPUT_ROOT:-$(realpath "$GCV2_RUNTIME_LIB_DIR/../..")}
uptime > "$OUT/uptime-before.txt"
start=$SECONDS
sha256sum "$LLC" "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" > "$OUT/inputs.sha256"
"$LLC" --cangjie-pipeline -mtriple=x86_64-unknown-linux-gnu -filetype=obj "$src/return_poll_pair.ll" -o "$OUT/return.o"
"${CXX:-clang++}" -std=gnu++17 -O0 -g -pthread -fno-rtti -fno-omit-frame-pointer -fvisibility-inlines-hidden \
 -I"$root/runtime/src" -I"$root/runtime/src/Loader/BinaryFile" -I"$root/runtime/src/Heap" \
 -I"$root/runtime/src/Heap/z/os/linux" -I"$root/runtime/src/CJThread/src/runtime/schedule/include" \
 -I"$root/runtime/include" -I"$root/runtime/third_party/third_party_bounds_checking_function/include" \
 -I"$headers/include" "$src/return_poll_pair.cpp" "$OUT/return.o" \
 -L"$GCV2_RUNTIME_LIB_DIR" -Wl,-rpath,"$GCV2_RUNTIME_LIB_DIR" -lcangjie-runtime -lboundscheck -o "$OUT/return-pair"
sha256sum "$OUT/return-pair" "$OUT/return.o" > "$OUT/outputs.sha256"
set +e
LD_LIBRARY_PATH="$GCV2_RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" timeout 30 "$OUT/return-pair" > "$OUT/run.log" 2>&1
rc=$?
set -e
echo "$rc" > "$OUT/run.rc"
uptime > "$OUT/uptime-after.txt"
echo "RETURN_PAIR_RC=$rc wall=$((SECONDS-start))"
cat "$OUT/run.log"
exit "$rc"
