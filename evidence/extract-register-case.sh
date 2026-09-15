#!/bin/bash
set -u
ulimit -c 0
root=/root/sym_cangjie_runtime_608_implement_r5676392826/llvm
cd "$root"
start=$SECONDS
ninja -C llvm-build -j"$(nproc)" llvm-extract > evidence/extract-tool-build.log 2>&1
echo $? > evidence/extract-tool-build.rc
llvm-build/bin/llvm-extract --func=_CNat5ArrayIG_E4swapHll stdlib/build/build/modules/linux_x86_64_cjnative/std/std.core-temp-files/std.core.opt.bc -o evidence/p01-array-swap.bc > evidence/extract-case.log 2>&1
echo $? > evidence/extract-case.rc
llvm-build/bin/llc evidence/p01-array-swap.bc --cangjie-pipeline --relocation-model=pic --frame-pointer=non-leaf --stack-trace-format=default -mcpu=generic -mattr=-avx --cj-safepoint-outline=false -O2 --filetype=asm -o evidence/p01-array-swap.s > evidence/p01-array-swap.old.log 2>&1
echo $? > evidence/p01-array-swap.old.rc
llvm-build/bin/llc evidence/p01-array-swap.bc --cangjie-pipeline --relocation-model=pic --frame-pointer=non-leaf --stack-trace-format=default -mcpu=generic -mattr=-avx --cj-safepoint-outline=false -O2 -stop-before=greedy -o evidence/p01-array-swap.mir > evidence/p01-array-swap.mir.log 2>&1
echo $? > evidence/p01-array-swap.mir.rc
sha256sum llvm-build/bin/llc evidence/p01-array-swap.bc evidence/p01-array-swap.mir > evidence/p01-array-swap.old.sha256
echo "$((SECONDS-start))" > evidence/p01-array-swap.wall
