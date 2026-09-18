set pagination off
set confirm off
set environment LD_LIBRARY_PATH /root/diff_379aae5a86ca/default/build/runtime-staging/lib/x86_64_Release
set environment GC_UNIT_FILTER LoadFc.SwapOldValueHealthyTargetReturnsNormally
run --gtest_filter=LoadFc.SwapOldValueHealthyTargetReturnsNormally
info program
p/x $pc
info symbol $pc
thread apply all bt
info proc mappings
quit
