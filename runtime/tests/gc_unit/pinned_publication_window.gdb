set pagination off
set confirm off
set breakpoint pending on
set print thread-events off
handle SIGSEGV nostop noprint pass
handle SIGUSR2 nostop noprint pass
break zObjectAllocator.inline.hpp:39
commands
silent
disable 1
printf "GDB_PRODUCT_ACQUIRED_PAGE\n"
bt 5
call (void)PinnedWindowCollect((void*)region)
continue
end
run
if $_exitcode == 0
  quit 0
else
  quit 1
end
