import json
import os
import pathlib
import subprocess
import sys
import time

import pexpect


root = pathlib.Path(sys.argv[1]).resolve()
mode = sys.argv[2]
started = time.monotonic()
environment = dict(os.environ, PROGRESS_ROOT=str(root), PROGRESS_MODE=mode)
transcript = (root / "driver.log").open("w")
session = pexpect.spawn("gdb", ["-q", "-x", str(pathlib.Path(__file__).with_name("ordered.gdb"))],
                        env=environment, encoding="utf-8", timeout=30)
session.logfile = transcript
qualified = False
exited = False
result = {"mode": mode, "status": "HARNESS_ERROR", "rc": 2}


def command(value):
    global exited
    session.sendline(value)
    session.sendline('python print("COMMAND_FINISHED")')
    session.expect_exact("\r\nCOMMAND_FINISHED\r\n")
    exited = exited or "exited normally]" in session.before
    return session.before


try:
    if mode in ("asleep", "gap", "lifetime"):
        session.expect_exact("EVENT empty final check complete")
        if mode in ("asleep", "lifetime"):
            command("python gdb.execute('thread %d' % worker)")
            command("continue &")
            time.sleep(0.2)
            sample = command("python current = [entry for entry in gdb.selected_inferior().threads() if entry.num == worker][0]; print('WORKER_WCHAN', open('/proc/%d/task/%d/wchan' % (gdb.selected_inferior().pid, current.ptid[1])).read())")
            if "futex" not in sample:
                raise RuntimeError("worker not asleep before publication")
        command("python print('MAPS_BEGIN'); print(open('/proc/%d/maps' % gdb.selected_inferior().pid).read()); print('MAPS_END')")
        command("python gdb.execute('thread %d' % blocked)")
        session.sendline("continue &")
        session.expect_exact("EVENT publisher reached ThreadStop")
        if mode == "lifetime":
            time.sleep(0.2)
            state = command("python print('TASK_FREED_BEFORE_PUBLISHER_RESUME', task_freed)")
            if "TASK_FREED_BEFORE_PUBLISHER_RESUME True" not in state:
                raise RuntimeError("task reclamation did not precede publisher resume")
        command("disable breakpoints")
        command("continue -a &")
        qualified = True
    elif mode == "slow-p":
        session.expect_exact("EVENT release held")
        time.sleep(0.2)
        sample = command("python current = [entry for entry in gdb.selected_inferior().threads() if entry.num == worker][0]; print('WORKER_WCHAN', open('/proc/%d/task/%d/wchan' % (gdb.selected_inferior().pid, current.ptid[1])).read())")
        if "futex" not in sample:
            raise RuntimeError("slow-P helper not asleep")
        session.sendline("continue -a &")
        qualified = True
    else:
        qualified = True
    try:
        if not exited:
            session.expect_exact("exited normally]", timeout=5)
    except pexpect.TIMEOUT:
        if not qualified:
            raise
        target = (root / "target.log").read_text()
        if "ASSERT ordinary_task_completion_after_pipe_release PASS" in target:
            raise RuntimeError("task completed but debugger did not observe process exit")
        log = (root / "driver.log").read_text()
        if mode == "slow-p" and ("EVENT syscall-exit-slow" not in log or "EVENT actual-no-P-branch" in log):
            raise RuntimeError("slow-P branch qualification failed before deadline")
        result.update(status="FAIL", rc=1, assertion="ordinary_task_completion_after_pipe_release")
        print("ASSERT ordinary_task_completion_after_pipe_release FAIL mode=" + mode, flush=True)
    else:
        exit_status = command("p $_exitcode")
        if "= 0" not in exit_status:
            raise RuntimeError("target did not exit with rc=0")
        target = (root / "target.log").read_text()
        if "ASSERT ordinary_task_completion_after_pipe_release PASS" not in target:
            raise RuntimeError("product future/result assertion absent")
        session.sendline("quit")
        session.expect(pexpect.EOF)
        log = (root / "driver.log").read_text()
        if mode == "fast" and ("EVENT syscall-exit-slow" in log or "EVENT syscall-exit\n" not in log):
            raise RuntimeError("fast exit branch qualification failed")
        if mode == "slow-p" and ("EVENT syscall-exit-slow" not in log or "EVENT actual-no-P-branch" in log):
            raise RuntimeError("slow-P branch qualification failed")
        result.update(status="PASS", rc=0, assertion="ordinary_task_completion_after_pipe_release")
        print("ASSERT ordinary_task_completion_after_pipe_release PASS mode=" + mode, flush=True)
except Exception as error:
    result.update(status="HARNESS_ERROR", rc=2, error=str(error))
finally:
    if session.isalive():
        session.sendline("kill")
        session.sendline("quit")
        session.close(force=True)
    transcript.close()
    result.update(qualified=qualified, wall=time.monotonic() - started)
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
sys.exit(result["rc"])
