set pagination off
set confirm off
set breakpoint pending on
handle SIGSEGV nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGABRT nostop print pass
set environment ZVerifyMarking 1
set environment ZVerifyRoots 0
set environment ZVerifyRemembered 0
set environment ZVerifyObjects 0
start
break zMark.cpp:1144
commands
silent
if p16_mark != 0 && (void*)this == (void*)p16_mark
  printf "VERIFY_WORKER_COORDINATOR_REACHED owner=%p prepared=%p\n", p16_worker, p16_prepared
  bt 8
  info proc mappings
  if p16_worker->markStacks[1].stacks._M_impl._M_start[0] != 0
    echo VERIFY_WORKER_INPUT_UNQUALIFIED_nonempty_slot\n
    quit 81
  end
  if $inject
    set p16_worker->markStacks[1].stacks._M_impl._M_start[0] = p16_prepared
    printf "VERIFY_WORKER_INPUT_INSTALLED slot=%p stack=%p\n", &p16_worker->markStacks[1].stacks._M_impl._M_start[0], p16_prepared
  end
  disable 2
end
continue
end
continue
if $_isvoid($_exitcode)
  printf "VERIFY_WORKER_INFERIOR_SIGNAL=%d\n", $_exitsignal
  quit 134
else
  printf "VERIFY_WORKER_INFERIOR_RC=%d\n", $_exitcode
  quit $_exitcode
end
