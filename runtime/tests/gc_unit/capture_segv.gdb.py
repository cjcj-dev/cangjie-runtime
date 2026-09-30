"""GDB driver for the armed-root test; no inferior code or signal handler."""
import gdb


root = gdb.selected_inferior().num
root_status = None


def exited(event):
    global root_status
    if event.inferior.num == root:
        root_status = getattr(event, "exit_code", 1)


def stopped(event):
    if not isinstance(event, gdb.SignalEvent) or event.stop_signal != "SIGSEGV":
        return
    gdb.write("GC_UNIT_SIGSEGV_CAPTURE_BEGIN inferior=%d pid=%d\n" %
              (gdb.selected_inferior().num, gdb.selected_inferior().pid))
    for command in ("p $_siginfo", "info inferiors", "thread apply all bt full",
                    "p MapleRuntime::ZGeneration::_young->_phase",
                    "p MapleRuntime::ZGeneration::_old->_phase"):
        try:
            gdb.execute(command)
        except gdb.error as error:
            gdb.write("GC_UNIT_SIGSEGV_CAPTURE_UNAVAILABLE %s: %s\n" %
                      (command, error))
    gdb.write("GC_UNIT_SIGSEGV_CAPTURE_END\n")


gdb.events.exited.connect(exited)
gdb.events.stop.connect(stopped)
try:
    gdb.write("GC_UNIT_SIGSEGV_CAPTURE_READY\n")
    gdb.execute("run")
    while root_status is None:
        live = [inferior for inferior in gdb.inferiors() if inferior.pid]
        if not live:
            raise gdb.GdbError("root process exited without an exit event")
        if not gdb.selected_inferior().pid:
            gdb.execute("inferior %d" % live[0].num)
        gdb.execute("continue")
except gdb.error as error:
    gdb.write("GC_UNIT_SIGSEGV_CAPTURE_ERROR %s\n" % error)
    gdb.execute("quit 125")
gdb.execute("quit %d" % root_status)
