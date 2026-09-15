set breakpoint pending on
break CJ_MCC_PostWriteRefField
condition 1 $rsi == 0
run
printf "STACK_STORE ref=%#lx holder=%#lx slot=%#lx previous=%#lx\n", $rdi, $rsi, $rdx, $rcx
x/gx $rdx
bt 12
info proc mappings
