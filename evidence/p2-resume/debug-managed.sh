#!/bin/bash
set -u
ulimit -c 0
R=/root/sym_cangjie_runtime_607_implement_r5684610492-build3
export LD_LIBRARY_PATH="$R/managed:$R/testable/build/runtime-staging/lib/x86_64_Release:$R/sdk/runtime/lib/linux_x86_64_cjnative"
export cjGCInterval=3600s
taskset -c 112-119 gdb -q -batch -ex run -ex 'bt 18' --args "$R/managed/p2_field_barrier" > "$R/managed/backtrace.log" 2>&1
/usr/bin/grep -A22 'received signal' "$R/managed/backtrace.log"
