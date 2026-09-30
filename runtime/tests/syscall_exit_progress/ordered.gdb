set pagination off
set confirm off
set breakpoint pending on
set non-stop on
set print thread-events off
python
import os
import re
import gdb
root = os.environ["PROGRESS_ROOT"]
mode = os.environ["PROGRESS_MODE"]
gdb.execute("set environment LD_LIBRARY_PATH " + root + "/lib")
gdb.execute("file " + root + "/blocking_task")
gdb.execute("set args " + mode)
gdb.execute("set logging file " + root + "/gdb.log")
gdb.execute("set logging overwrite on")
gdb.execute("set logging enabled on")
gdb.execute("start " + mode + " > " + root + "/target.log 2>&1")
worker = None
blocked = None
phase = "initial"
slow_entries = 0
exit_entries = 0
blocked_task = None
task_freed = False

def resume_thread(number):
    def resume():
        gdb.execute("thread %d" % number)
        gdb.execute("continue &")
    gdb.post_event(resume)

class Helper(gdb.Breakpoint):
    def stop(self):
        global worker
        worker = gdb.selected_thread().num
        print("EVENT helper worker=%d" % worker, flush=True)
        return False

class Exit(gdb.Breakpoint):
    def stop(self):
        global exit_entries
        exit_entries += 1
        print("EVENT syscall-exit", flush=True)
        return False

class Slow(gdb.Breakpoint):
    def stop(self):
        global slow_entries, blocked_task
        slow_entries += 1
        blocked_task = int(gdb.parse_and_eval("$rdi"))
        print("EVENT syscall-exit-slow", flush=True)
        return False

class LastCheck(gdb.Breakpoint):
    def stop(self):
        global phase
        if gdb.selected_thread().num == worker and phase == "initial" and mode in ("asleep", "gap", "lifetime"):
            phase = "worker-held"
            print("EVENT worker held before release", flush=True)
            return True
        return False

class NoProcessor(gdb.Breakpoint):
    def stop(self):
        global blocked, phase
        print("EVENT actual-no-P-branch", flush=True)
        if phase == "worker-held":
            blocked = gdb.selected_thread().num
            phase = "queue-held"
            gdb.execute("bt 5")
            resume_thread(worker)
            return True
        return False

class Stop(gdb.Breakpoint):
    def stop(self):
        global phase
        number = gdb.selected_thread().num
        if number == worker and phase == "queue-held":
            phase = "window"
            print("EVENT empty final check complete", flush=True)
            gdb.execute("bt 4")
            return True
        if number == blocked and phase == "window":
            phase = "published"
            print("EVENT publisher reached ThreadStop", flush=True)
            gdb.execute("bt 4")
            return True
        return False

class Release(gdb.Breakpoint):
    def stop(self):
        if mode == "slow-p":
            print("EVENT release held", flush=True)
            return True
        return False

class Completed(gdb.Breakpoint):
    def stop(self):
        if mode in ("asleep", "gap", "lifetime"):
            print("EVENT future completed while publisher held", flush=True)
            return True
        return False

class Freed(gdb.Breakpoint):
    def stop(self):
        if blocked_task == int(gdb.parse_and_eval("$rdi")):
            print("EVENT blocking task reached CJThreadFree", flush=True)
            return_address = int(gdb.parse_and_eval("*(unsigned long*)$rsp"))
            returned = FreeReturned("*0x%x" % return_address, internal=True)
            returned.thread = gdb.selected_thread().num
            print("IDENTITY CJThreadFree-return=0x%x" % return_address, flush=True)
        return False

class FreeReturned(gdb.Breakpoint):
    def stop(self):
        global task_freed
        task_freed = True
        print("EVENT blocking task CJThreadFree returned", flush=True)
        return False

address = int(gdb.parse_and_eval("&CJ_SyscallExit0"))
instructions = gdb.selected_frame().architecture().disassemble(address, address + 512)
allocated = False
branch_address = None
for index, instruction in enumerate(instructions):
    assembly = instruction["asm"]
    if "call" in assembly and "CJ_ProcessorAlloc" in assembly:
        allocated = True
    elif allocated and re.match(r"j(e|ne|z|nz)\s", assembly):
        opcode = assembly.split()[0]
        branch_address = int(re.search(r"0x[0-9a-f]+", assembly).group(), 16) if opcode in ("je", "jz") else instructions[index + 1]["addr"]
        break
if branch_address is None:
    raise RuntimeError("no-P branch could not be located in this product")
print("IDENTITY no-P-instruction=0x%x" % branch_address, flush=True)
gdb.execute("disassemble CJ_SyscallExit0")
Helper("OrdinaryHelperTask")
Exit("CJ_SyscallExit")
Slow("CJ_SyscallExit0")
LastCheck("CJ_ProcessorStopWithLastCheck")
NoProcessor("*0x%x" % branch_address)
Stop("CJ_ThreadStop")
Release("ReleasePipe")
if mode in ("asleep", "gap", "lifetime"):
    Completed("_Exit")
if mode == "lifetime":
    Freed("*CJ_CJThreadFree")
if mode == "ordinary":
    gdb.execute("disable breakpoints")
print("EVENT controller ready", flush=True)
end
continue -a &
