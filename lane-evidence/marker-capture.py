import gdb

def out(s): print(s, flush=True)
gdb.execute('set pagination off')
gdb.execute('set confirm off')
gdb.execute('set breakpoint pending on')
gdb.execute('set follow-fork-mode child')
gdb.execute('set detach-on-fork on')
gdb.execute('handle SIGSEGV nostop noprint pass')
class Returned(gdb.FinishBreakpoint):
    def __init__(self, frame, slot, before):
        super().__init__(frame, internal=True)
        self.slot, self.before = slot, before
    def stop(self):
        after=int.from_bytes(gdb.selected_inferior().read_memory(self.slot,8),'little')
        out('NATIVE_READ_RESULT slot=%#x before=%#x after=%#x returned=%s' % (self.slot,self.before,after,self.return_value))
        return False
class Read(gdb.Breakpoint):
    def stop(self):
        try:
            slot=int(gdb.parse_and_eval('&field'))
            before=int.from_bytes(gdb.selected_inferior().read_memory(slot,8),'little')
            Returned(gdb.newest_frame(),slot,before)
        except Exception as e: out('CAPTURE_ERROR read %s' % e)
        return False
class Push(gdb.Breakpoint):
    maps=False
    def stop(self):
        try:
            bits=int(gdb.parse_and_eval('entry.entry'))
            out('MARKER_ARGUMENT object=%#x bits=%#x' % (bits>>5,bits))
            out(gdb.execute('bt 8',to_string=True))
            if not self.maps:
                out(gdb.execute('info proc mappings',to_string=True))
                self.maps=True
        except Exception as e: out('CAPTURE_ERROR push %s' % e)
        return False
Read('MapleRuntime::Barrier::ReadStaticRef')
Push('MapleRuntime::MarkThreadLocalStacks::Push')
gdb.execute('run')
out(gdb.execute('info program',to_string=True))
