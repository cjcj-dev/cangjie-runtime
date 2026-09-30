"""Non-stop controller for the existing lifecycle observer; no product hooks."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import pexpect

root, elf, lib, name, target = sys.argv[1:]
root = Path(root)
root.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, LD_LIBRARY_PATH=lib, GC_UNIT_FILTER=name,
           GC_UNIT_OTHER_VM_CHILD=name, SHUTDOWN1459_HOLD_TARGET=target)
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
    session.expect_exact("EVENT TLS_HELD")
    event = session.expect_exact(["EVENT JOIN_WITH_HELD", "EVENT TEARDOWN_WITH_HELD"])
    if event == 0:
        # One controlled observation after letting the caller enter libc join.
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
