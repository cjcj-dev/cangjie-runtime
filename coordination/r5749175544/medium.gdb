set pagination off
set breakpoint pending on
set follow-fork-mode child
set detach-on-fork on
set environment GC_UNIT_FILTER ObjectAllocatorPaths.MediumBlockingFailureAttemptsCollection
set environment LD_LIBRARY_PATH /root/sym_cangjie_runtime_759_implement_r5749175544-initial/testable/build/runtime-staging/lib/x86_64_Release
handle SIGSEGV nostop noprint pass
break MapleRuntime::Heap::free_empty_pages
commands
silent
printf "EMPTY_PAGE\n"
p id
p *pages
p pages->_array[0]
p *pages->_array[0]
bt 5
continue
end
run
bt 12
