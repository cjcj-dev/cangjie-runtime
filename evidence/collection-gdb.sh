#!/bin/bash
set -u
ulimit -c 0
root=/root/sym_cangjie_runtime_608_implement_r5676392826/llvm
cd "$root/stdlib/build/build/libs"
export CANGJIE_HOME="$root/target" cjHeapSize=24GB
export LD_LIBRARY_PATH="$root/host/runtime/lib/linux_x86_64_cjnative:$root/host/third_party/llvm/lib:$root/host/tools/lib"
export PATH="$CANGJIE_HOME/bin:$CANGJIE_HOME/tools/bin:$CANGJIE_HOME/third_party/llvm/bin:/usr/bin:/bin"
export CANGJIE_PATH="$root/stdlib/build/build/modules/linux_x86_64_cjnative" LIBRARY_PATH="$root/stdlib/build/build/lib"
gdb -q -batch -ex run -ex 'bt 25' -ex 'info proc mappings' --args "$root/target/bin/cjc" -g --apc=1 --output-type=staticlib -p "$root/stdlib/libs/std/collection" --output "$root/stdlib/build/build/modules/linux_x86_64_cjnative/std/std.collection.a" -j1 --save-temps="$root/stdlib/build/build/modules/linux_x86_64_cjnative/std/std.collection-temp-files" -O2 > "$root/evidence/collection-r4-gdb.log" 2>&1
echo $? > "$root/evidence/collection-r4-gdb.rc"
