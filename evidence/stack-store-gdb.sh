#!/bin/bash
set -u
ulimit -c 0
root=/root/sym_cangjie_runtime_608_implement_r5676392826-nested-tests
cd "$root"
export LD_LIBRARY_PATH="$root/testable/build/runtime-staging/lib/x86_64_Release:$root/gate-sdk/runtime/lib/linux_x86_64_cjnative"
export cjHeapSize=256MB
gdb -q -batch -ex 'set breakpoint pending on' -ex 'break MCC_WriteRefField if obj == 0' -ex run -ex 'bt 12' -ex 'set $observed_slot = field' -ex 'p obj' -ex 'p field' -ex 'p ref' -ex 'x/gx $observed_slot' -ex finish -ex 'x/gx $observed_slot' -ex 'info proc mappings' --args managed/finalizer_trigger-1/finalizer_trigger > evidence/stack-store-gdb.log 2>&1
echo $? > evidence/stack-store-gdb.rc
