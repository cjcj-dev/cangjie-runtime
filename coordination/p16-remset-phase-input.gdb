set pagination off
set confirm off
set breakpoint pending on
handle SIGSEGV nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGABRT nostop print pass
set environment ZVerifyRemembered 1
set environment ZVerifyRoots 0
set environment ZVerifyMarking 0
set environment ZVerifyForwarding 0
start
break MapleRuntime::ZVerify::AfterRelocationInternal
commands
silent
if forwarding == p16_forwarding
  printf "VERIFY_REMSET_CONSUMER_REACHED forwarding=%p field=%p word=%#lx\n", forwarding, p16_destination_field, *p16_destination_field
  bt 9
  info proc mappings
  if (*p16_destination_field & p16_remset_mask) != p16_remset_mask
    echo VERIFY_REMSET_INPUT_UNQUALIFIED_color\n
    quit 81
  end
  if $inject
    set *p16_destination_field = *p16_destination_field & ~p16_remset_mask
    printf "VERIFY_REMSET_INPUT_INSTALLED field=%p word=%#lx\n", p16_destination_field, *p16_destination_field
  end
  disable 2
end
continue
end
continue
if $_isvoid($_exitcode)
  printf "VERIFY_REMSET_INFERIOR_SIGNAL=%d\n", $_exitsignal
  quit 134
else
  printf "VERIFY_REMSET_INFERIOR_RC=%d\n", $_exitcode
  quit $_exitcode
end
