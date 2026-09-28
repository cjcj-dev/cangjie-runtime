# Observe the first real ThrowException producing pending state, then enter
# the same compiler ABI a second time. Never write ExceptionWrapper fields.
import gdb
for command in ["set pagination off", "set confirm off", "set breakpoint pending on",
                "set print thread-events off", "handle SIGABRT nostop noprint pass",
                "handle SIGUSR1 nostop noprint pass", "handle SIGUSR2 nostop noprint pass"]:
    gdb.execute(command)
try:
    entry = gdb.Breakpoint("MapleRuntime::ExceptionHandling::BuildEHFrameInfo()")
    gdb.execute("run")
    entry.delete()
    produced = int(gdb.parse_and_eval("this->eWrapper->exceptionRef"))
    print("EXCEPTION_PRODUCER_TARGET pending_nonnull=%d" % (produced != 0), flush=True)
    if produced == 0:
        gdb.execute("quit 5")
    try:
        gdb.execute("call (void) SignalThrowAgain()")
    except gdb.error as error:
        print("EXCEPTION_CALL_RETURN " + str(error), flush=True)
    terminated = gdb.selected_inferior().pid == 0
    print("EXCEPTION_EXIT_TARGET terminated=%d" % terminated, flush=True)
    gdb.execute("quit 0" if terminated else "quit 4")
except gdb.error as error:
    print("GDB_ERROR " + str(error), flush=True)
    gdb.execute("quit 4")
