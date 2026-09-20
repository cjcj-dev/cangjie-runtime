set pagination off
set breakpoint pending on
set follow-fork-mode child
set detach-on-fork on
set environment GC_UNIT_FILTER ObjectAllocatorPaths.MediumBlockingFailureAttemptsCollection
set environment LD_LIBRARY_PATH /root/sym_cangjie_runtime_759_implement_r5749175544-initial/testable/build/runtime-staging/lib/x86_64_Release
handle SIGSEGV nostop noprint pass
break MapleRuntime::Heap::free_page
commands
silent
printf "FREE_PAGE\n"
info args
p *page
bt 6
continue
end
break MapleRuntime::RegionManager::ReleaseRegion
commands
silent
printf "RELEASE_REGION\n"
info args
bt 6
continue
end
run
bt 16
