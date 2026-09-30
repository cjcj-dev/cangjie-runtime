"""Observe stopped inferiors without calling into the tested program."""
import hashlib
import os
import gdb

root = gdb.selected_inferior().num
root_status = None
captured = set()


def exited(event):
    global root_status
    number = event.inferior.num
    status = getattr(event, "exit_code", None)
    gdb.write("GC_UNIT_SIGSEGV_INFERIOR_EXIT inferior=%d exit_code=%s captured=%s\n" %
              (number, status if status is not None else "SIGNAL_OR_UNAVAILABLE",
               "yes" if number in captured else "no"))
    if number == root:
        root_status = status if status is not None else 1


def identity(pid):
    # Hash exactly the files mapped by this stopped process, including its ELF.
    with open("/proc/%d/maps" % pid) as stream:
        maps = stream.read()
    gdb.write("GC_UNIT_SIGSEGV_MAPS_BEGIN pid=%d\n%sGC_UNIT_SIGSEGV_MAPS_END\n" % (pid, maps))
    paths = {"/proc/%d/exe" % pid}
    for line in maps.splitlines():
        fields = line.split(None, 5)
        if len(fields) == 6 and fields[5].startswith("/"):
            paths.add(fields[5])
    for path in sorted(paths):
        try:
            digest = hashlib.sha256()
            with open(path, "rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
            gdb.write("GC_UNIT_SIGSEGV_BINARY sha256=%s path=%s resolved=%s\n" %
                      (digest.hexdigest(), path, os.path.realpath(path)))
        except OSError as error:
            gdb.write("GC_UNIT_SIGSEGV_BINARY_UNAVAILABLE path=%s error=%s\n" % (path, error))


def stopped(event):
    if not isinstance(event, gdb.SignalEvent) or event.stop_signal != "SIGSEGV":
        return
    inferior = gdb.selected_inferior()
    thread = gdb.selected_thread()
    captured.add(inferior.num)
    gdb.write("GC_UNIT_SIGSEGV_CAPTURE_BEGIN inferior=%d pid=%d tid=%s\n" %
              (inferior.num, inferior.pid, thread.ptid))
    failed = False
    for command in ("p $_siginfo", "info inferiors", "info threads",
                    "thread apply all bt full", "bt full", "info registers",
                    "info sharedlibrary", "info proc exe",
                    "p MapleRuntime::ZGeneration::_young->_phase",
                    "p MapleRuntime::ZGeneration::_old->_phase"):
        try:
            gdb.write("GC_UNIT_SIGSEGV_COMMAND %s\n" % command)
            gdb.execute(command)
        except gdb.error as error:
            if command in ("p $_siginfo", "thread apply all bt full", "info registers"):
                failed = True
            gdb.write("GC_UNIT_SIGSEGV_CAPTURE_UNAVAILABLE %s: %s\n" % (command, error))
    try:
        identity(inferior.pid)
    except OSError as error:
        failed = True
        gdb.write("GC_UNIT_SIGSEGV_CAPTURE_UNAVAILABLE maps: %s\n" % error)
    gdb.write("GC_UNIT_SIGSEGV_CAPTURE_STATUS inferior=%d status=%s\n" %
              (inferior.num, "FAILED" if failed else "COMPLETE"))
    if failed:
        gdb.write("GC_UNIT_SIGSEGV_CAPTURE_FAILED incomplete stopped-process evidence\n")
    gdb.write("GC_UNIT_SIGSEGV_CAPTURE_END\n")


gdb.events.exited.connect(exited)
gdb.events.stop.connect(stopped)
try:
    gdb.write("GC_UNIT_SIGSEGV_CAPTURE_READY\n")
    gdb.execute("run")
    # Drain descendants too: root completion must not hide a live inferior.
    while any(inferior.pid for inferior in gdb.inferiors()):
        if not gdb.selected_inferior().pid:
            live = next(inferior for inferior in gdb.inferiors() if inferior.pid)
            gdb.execute("inferior %d" % live.num)
        gdb.execute("continue")
    if root_status is None:
        raise gdb.GdbError("root process exited without an exit event")
except gdb.error as error:
    gdb.write("GC_UNIT_SIGSEGV_CAPTURE_FAILED %s\n" % error)
    gdb.execute("quit 125")
gdb.write("GC_UNIT_SIGSEGV_ROOT_STATUS exit_code=%d captured_inferiors=%d\n" %
          (root_status, len(captured)))
gdb.execute("quit %d" % root_status)
