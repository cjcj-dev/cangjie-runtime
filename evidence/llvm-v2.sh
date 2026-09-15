#!/bin/bash
ulimit -c 0
root=/root/sym_cangjie_runtime_608_implement_r5676392826/llvm
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR="$root/llvm-src" CCACHE_NOHASHDIR=1
start=$SECONDS
ninja -C "$root/llvm-build" -j"$(nproc)" -l150 llc opt FileCheck > "$root/evidence/llvm-v2.log" 2>&1
rc=$?
echo "$rc" > "$root/evidence/llvm-v2.rc"
echo "$((SECONDS-start))" > "$root/evidence/llvm-v2.wall"
if [[ $rc == 0 ]]; then sha256sum "$root/llvm-build/bin/llc" "$root/llvm-build/bin/opt" > "$root/evidence/llvm-v2.sha256"; fi
