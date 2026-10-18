"""Non-stop controller for the existing lifecycle observer; no product hooks."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import pexpect

root, elf, lib, name, target = sys.argv[1:6]
boundary = sys.argv[6] if len(sys.argv)>6 else ""
root = Path(root)
root.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, LD_LIBRARY_PATH=lib, GC_UNIT_FILTER=name,
           GC_UNIT_OTHER_VM_CHILD=name, SHUTDOWN1459_HOLD_TARGET=target, SHUTDOWN1459_BOUNDARY=boundary)
observer = Path(__file__).with_name("shutdown_1459_lifecycle.py")
args = ["-q", "-ex", "set pagination off", "-ex", "set confirm off",
        "-ex", "set non-stop on", "-ex", "start", "-ex", "source " + str(observer),
        "-ex", "continue -a &", elf]
transcript = (root / "driver.log").open("w")
session = pexpect.spawn("gdb", args, env=env, encoding="utf-8", timeout=30)
session.logfile = transcript
result = dict(status="HARNESS_ERROR", rc=2, qualified=False)
started = time.monotonic()

def command(text):
    session.sendline(text)
    session.sendline('python print("COMMAND_FINISHED")')
    session.expect_exact("\r\nCOMMAND_FINISHED\r\n")
    return session.before

try:
    session.expect_exact("EVENT controller ready")
    if boundary and boundary != "late_lock":
        event=session.expect_exact(["EVENT PRODUCER_BOUNDARY_RED", "EVENT CONSUMER_RESTORE_RED", "exited normally]"], timeout=30)
        if event == 2:
            verdict=command("python print('BOUNDARY_VERDICT',len(failures),teardown_seen,bool(tls_completed),len(joined))")
            if "BOUNDARY_VERDICT 0 True True" not in verdict:
                raise RuntimeError("normal shutdown boundary invariant failed")
            result.update(status="PASS",rc=0,qualified=True,assertion="normal_exit_context_and_resources_before_teardown")
        else:
            verdict=command("python print('BOUNDARY_VERDICT',boundary_qualified)")
            if "BOUNDARY_VERDICT True" not in verdict:
                raise RuntimeError("boundary red has no qualified product state")
            result.update(status="FAIL",rc=1,qualified=True,assertion=("bootstrap_completed_before_runtime_delete" if event==0 else "native_exit_context_restored"))
    else:
        event=session.expect_exact(["EVENT FINI_HELD", "EVENT TLS_HELD"])
        if event==0:
            terminal=command("python observe_held_terminal()")
            if "TERMINAL_OBSERVED 0" not in terminal:
                raise RuntimeError("logical terminal state was not observed")
            armed=command("python begin_late(%s)"%("True" if boundary=="late_lock" else "False"))
            if "LATE_ARMED 1" not in armed:
                raise RuntimeError("late return was not armed")
            token="EVENT RDLOCK_CUT" if boundary=="late_lock" else "EVENT RDLOCK_ENTERED"
            if token not in armed:
                session.expect_exact(token, timeout=10)
                armed += session.before or ""
            if boundary=="late_lock":
                if "received signal SIGABRT" not in armed:
                    session.expect_exact("received signal SIGABRT", timeout=20)
                sample=command("python observe_barrier_abort()")
                if "EVENT LATE_BARRIER_ABORT" not in sample:
                    raise RuntimeError("SIGABRT was not the permanent barrier return check")
                transcript.flush()
                if "returned through permanent VM exit barrier" not in (root / "driver.log").read_text():
                    raise RuntimeError("permanent barrier CHECK diagnostic was not emitted")
                result.update(status="FAIL", rc=1, qualified=True, assertion="permanent_barrier_return_check_sigabrt")
            else:
                time.sleep(0.2)
                command("python note_running()")
                time.sleep(0.2)
                command("python note_running()")
                verdict=command("python finish_late(False)")
                numbers=command("python print('LATE_VERDICT',len(failures),int(late_parked),int(cut_skipped),int(rdlock_entered))")
                if "EVENT LATE_RETURN_BLOCKED" not in verdict or "LATE_VERDICT 0 1 0 1" not in numbers:
                    raise RuntimeError("late thread was not held by the permanent lock")
                # Release the held Fini caller and require the product fixture
                # to complete; killing a paused caller cannot stand in for exit.
                command("python gdb.execute('thread %d'%fini_thread)")
                session.sendline("continue &")
                session.expect_exact("exited normally]", timeout=30)
                completion=command("python print('LATE_EXIT_VERDICT',len(failures),blocked_result_seen,late_parked,bool(tls_completed))")
                if "LATE_EXIT_VERDICT 0 True True True" not in completion:
                    raise RuntimeError("late-return control failed after releasing Fini")
                result.update(status="PASS", rc=0, qualified=True, assertion="late_return_waits_on_permanent_rwlock")
        else:
            state = command("python print('CONTROLLER_STATE',gate_thread,joiner_thread,teardown_seen)")
            match = re.search(r"CONTROLLER_STATE (\d+) (\d+) (True|False)",state)
            if match is None:
                raise RuntimeError("controller state not observed")
            gate, joiner, teardown = match.groups()
            if gate != "0":
                command("python gdb.execute('thread %d'%gate_thread)")
                session.sendline("continue &")
                event = session.expect_exact(["EVENT JOIN_WITH_HELD", "EVENT TEARDOWN_WITH_HELD"])
            elif joiner != "0":
                event = 0
            elif teardown == "True":
                event = 1
            else:
                event = session.expect_exact(["EVENT JOIN_WITH_HELD", "EVENT TEARDOWN_WITH_HELD"])
            if event == 0:
                time.sleep(0.2)
                sample = command("python t=[t for t in gdb.selected_inferior().threads() if t.num==joiner_thread][0]; print('JOIN_WCHAN',open('/proc/%d/task/%d/wchan'%(gdb.selected_inferior().pid,t.ptid[1])).read()); print('TEARDOWN_WHILE_HELD',teardown_seen)")
                if "futex" not in sample or "TEARDOWN_WHILE_HELD False" not in sample:
                    raise RuntimeError("caller did not wait for held native TLS")
                command("python gdb.execute('thread %d'%held_thread)")
                session.sendline("continue &")
                session.expect_exact("exited normally]", timeout=30)
                verdict = command("python print('HOLD_VERDICT',len(failures),teardown_seen,held_tid in tls_completed)")
                if "HOLD_VERDICT 0 True True" not in verdict:
                    raise RuntimeError("lifecycle target failed after release")
                result.update(status="PASS", rc=0, qualified=True,
                              assertion="join_waits_for_actual_tls_before_teardown")
            else:
                verdict = command("python print('HELD_TARGET_RED',held_tid not in tls_completed,any(r['tid']==held_tid and not r['destroyed'] for r in records.values())); print('OTHER_OWNERS',all(r['destroyed'] for r in records.values() if r['tid'] in carriers.values() and r['tid']!=held_tid))")
                if "HELD_TARGET_RED True True" not in verdict or "OTHER_OWNERS True" not in verdict:
                    raise RuntimeError("target red is masked or captures other owners")
                result.update(status="FAIL", rc=1, qualified=True,
                              assertion="join_waits_for_actual_tls_before_teardown")
except Exception as error:
    result["error"] = str(error)
finally:
    if session.isalive():
        session.sendline("kill")
        session.sendline("quit")
        session.close(force=True)
    transcript.close()
    result["wall"] = time.monotonic() - started
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, sort_keys=True))
sys.exit(result["rc"])
