#!/usr/bin/env python3
"""Observe the real API startup handshake using gdb, without product hooks."""

import os
import sys


def observe():
    import gdb

    state = {"bound": 0, "ready": 0, "failed": False, "injected": False,
             "starter_returned": False, "main": None, "destroyed": False,
             "gated": False, "main_handle": 0, "sub": 0, "notifications": 0,
             "starter_parked": False}

    def target(name, condition, detail):
        state["failed"] |= not condition
        print("STARTUP_TARGET %s verdict=%s %s" %
              (name, "PASS" if condition else "FAIL", detail), flush=True)

    def ready():
        return bool(gdb.parse_and_eval("g_runtimeInited._M_base._M_i"))

    class Published(gdb.FinishBreakpoint):
        def stop(self):
            state["ready"] += 1
            target("READY_PUBLICATION", ready(), "ready=%d" % ready())
            return mode == "published"

    class Publish(gdb.Breakpoint):
        def stop(self):
            running = int(gdb.parse_and_eval("*(int*)scheduler"))
            handle = int(gdb.parse_and_eval("scheduler"))
            if mode == "isolation" and state["main_handle"] and handle != state["main_handle"]:
                target("SUB_RUNNING", running == 1 and ready(),
                       "schedule_state=%d main_ready=%d" % (running, ready()))
                SubPublished(gdb.newest_frame(), internal=True)
                return False
            state["main_handle"] = handle
            target("RUNNING_BEFORE_READY", running == 1 and not ready(),
                   "schedule_state=%d ready=%d" % (running, ready()))
            Published(gdb.newest_frame(), internal=True)
            return state["failed"]

    class SubPublished(gdb.FinishBreakpoint):
        def stop(self):
            state["sub"] += 1
            target("SUB_CANNOT_PUBLISH_MAIN_READY", ready() and state["notifications"] == 1,
                   "main_ready=%d global_notifications=%d" % (ready(), state["notifications"]))
            return False

    class GlobalNotify(gdb.Breakpoint):
        def stop(self):
            address = int(gdb.parse_and_eval("&g_conditionVariable"))
            if int(gdb.parse_and_eval("$rdi")) == address:
                state["notifications"] += 1
            return False

    class Bind(gdb.Breakpoint):
        def stop(self):
            state["bound"] += 1
            bound_handle = int(gdb.parse_and_eval("$rdi"))
            running = int(gdb.parse_and_eval("*(int*)$rdi"))
            target("CONSUMER_READY", ready() and running == 1
                   and bound_handle == state["main_handle"],
                   "ready=%d schedule_state=%d handle_match=%d observed_returns=%d" %
                   (ready(), running, bound_handle == state["main_handle"], state["ready"]))
            return mode == "published" or (mode in ("ordered", "notify") and not ready())

    class Fail(gdb.Breakpoint):
        def stop(self):
            state["injected"] = True
            target("FAILURE_NOT_READY", not ready(), "ready=%d" % ready())
            return True

    class StarterCompleted(gdb.FinishBreakpoint):
        def stop(self):
            state["starter_returned"] = True
            target("FAILURE_TERMINATES" if mode == "failure" else "INIT_COMPLETES",
                   False, "starter_returned_before_ready=%d" % (not ready()))
            return True

    class Starter(gdb.Breakpoint):
        def stop(self):
            if mode in ("ordered", "published", "notify"):
                starter_exit.condition = "$_thread == %d" % gdb.selected_thread().global_num
                starter_exit.enabled = True
            else:
                StarterCompleted(gdb.newest_frame(), internal=True)
            state["starter_parked"] = True
            return mode in ("ordered", "published", "notify")

    class InitGate(gdb.Breakpoint):
        def stop(self):
            state["main"] = gdb.selected_thread()
            gdb.execute("set scheduler-locking on")
            return False

    class DestroyGate(gdb.Breakpoint):
        def stop(self):
            if gdb.selected_thread() != state["main"]:
                return False
            state["destroyed"] = True
            return True

    class ScheduleGate(gdb.Breakpoint):
        def stop(self):
            state["gated"] = True
            return True

    gdb.execute("set pagination off")
    gdb.execute("set confirm off")
    gdb.execute("set breakpoint pending on")
    gdb.execute("set follow-fork-mode child")
    gdb.execute("set detach-on-fork on")
    gdb.execute("handle SIGUSR1 nostop noprint pass")
    gdb.execute("handle SIGUSR2 nostop noprint pass")
    gdb.execute("start")
    if "i386:x86-64" not in gdb.newest_frame().architecture().name():
        print("STARTUP_RESULT NOT_RUN supported envelope is Linux x86-64", flush=True)
        gdb.execute("quit 2")
    Publish("NotifyRuntimeSchedulerReady", internal=True)
    Bind("CJ_ScheduleSetToCurrentThread", internal=True)
    mode = os.environ["STARTUP_MODE"]
    if mode in ("ordered", "published", "notify"):
        gdb.execute("catch syscall exit")
        starter_exit = gdb.breakpoints()[-1]
        starter_exit.enabled = False

        def starter_exiting(event):
            if isinstance(event, gdb.BreakpointEvent) and starter_exit in event.breakpoints:
                state["starter_returned"] = True
                target("INIT_COMPLETES", False,
                       "starter_exit_syscall_before_ready=%d ready=%d" % (not ready(), ready()))

        gdb.events.stop.connect(starter_exiting)
    Starter("StartCJRuntime", internal=True)
    if mode == "isolation":
        GlobalNotify("*_ZNSt18condition_variable10notify_allEv", internal=True)
    if mode == "failure":
        Fail("CJ_SchmonStart", internal=True)
    elif mode in ("ordered", "published", "notify"):
        InitGate("InitCJRuntime", internal=True)
        destroy_gate = DestroyGate("pthread_attr_destroy", internal=True)
        schedule_gate = ScheduleGate("CJ_ScheduleStart", internal=True)
    gdb.execute("continue")
    if mode in ("ordered", "published", "notify"):
        if not state["destroyed"] and state["starter_parked"]:
            state["main"].switch()
            gdb.execute("continue")
        if not state["destroyed"]:
            target("ORDERING_GATE", False, "pthread_attr_destroy_not_reached")
            gdb.execute("quit 1")
        destroy_gate.delete()
        gdb.execute("finish")
        starters = [thread for thread in gdb.selected_inferior().threads()
                    if thread != state["main"]]
        target("ORDERING_GATE", len(starters) == 1,
               "starter_threads=%d" % len(starters))
        if len(starters) != 1:
            gdb.execute("quit 2")
        starters[0].switch()
        if not state["starter_parked"]:
            gdb.execute("continue")
        mutex_rc = int(gdb.parse_and_eval("(int)pthread_mutex_lock((void*)&g_mtx)"))
        target("CONSUMER_HELD_DURING_CONSTRUCTION", mutex_rc == 0, "mutex_rc=%d" % mutex_rc)
        gdb.execute("set scheduler-locking off")
        gdb.execute("continue")
        if state["starter_returned"] or not state["gated"]:
            gdb.execute("kill")
            gdb.execute("quit 1")
        starter_exit.delete()
        gdb.events.stop.disconnect(starter_exiting)
        schedule_gate.delete()
        gdb.execute("set scheduler-locking on")
        mutex_rc = int(gdb.parse_and_eval("(int)pthread_mutex_unlock((void*)&g_mtx)"))
        target("CONSTRUCTION_GATE_RELEASED", mutex_rc == 0, "mutex_rc=%d" % mutex_rc)
        if mode == "published":
            gdb.execute("continue")
        if mode == "notify":
            result = int(gdb.parse_and_eval(
                "(int)pthread_cond_broadcast((void*)&g_conditionVariable)"))
            target("NOTIFY_WITHOUT_READY", result == 0 and not ready(),
                   "notify_rc=%d ready=%d" % (result, ready()))
        state["main"].switch()
        wait_gate = None
        if mode != "published":
            gdb.execute("catch syscall futex")
            wait_gate = gdb.breakpoints()[-1]
        gdb.execute("continue")
        if mode == "published":
            target("READY_BEFORE_WAIT", ready() and state["bound"] == 1,
                   "ready=%d bindings=%d" % (ready(), state["bound"]))
        else:
            for stop_index in range(8):
                trace = gdb.execute("bt", to_string=True)
                if state["bound"] or "condition_variable" in trace:
                    break
                print("STARTUP_CONTROL prior_futex_stop=%d\n%s" % (stop_index, trace), flush=True)
                gdb.execute("continue")
            target("CONSUMER_WAITS_FOR_READY", not ready() and state["bound"] == 0
                   and "condition_variable" in trace and "InitCJRuntime" in trace,
                   "ready=%d bindings=%d\n%s" % (ready(), state["bound"], trace))
            wait_gate.delete()
        if state["failed"]:
            gdb.execute("kill")
            gdb.execute("quit 1")
        gdb.execute("set scheduler-locking off")
        gdb.execute("continue")
    if state["failed"]:
        if gdb.selected_inferior().threads():
            gdb.execute("kill")
        gdb.execute("quit 1")
    if mode == "failure" and state["injected"]:
        gdb.execute("return (int)11")
        gdb.execute("continue")
        if state["starter_returned"]:
            gdb.execute("kill")
            gdb.execute("quit 1")
        signal = str(gdb.parse_and_eval("$_siginfo.si_signo"))
        target("FAILURE_TERMINATES", signal == "6" and state["bound"] == 0
               and state["ready"] == 0,
               "signal=%s bindings=%d publications=%d" %
               (signal, state["bound"], state["ready"]))
        gdb.execute("kill")
    elif mode in ("success", "ordered", "published", "notify", "isolation"):
        if state["starter_returned"]:
            gdb.execute("kill")
            gdb.execute("quit 1")
        inferior_rc = int(gdb.parse_and_eval("$_exitcode"))
        target("INIT_COMPLETES", inferior_rc == 0 and state["bound"] > 0
               and state["ready"] == 1,
               "inferior_rc=%d bindings=%d publications=%d" %
               (inferior_rc, state["bound"], state["ready"]))
        if mode == "isolation":
            target("SUB_PATH_COMPLETES", state["sub"] == 1 and state["notifications"] == 1,
                   "sub_returns=%d global_notifications=%d" %
                   (state["sub"], state["notifications"]))
    else:
        target("INJECTION_REACHED", False, "injected=0")
    gdb.execute("quit %d" % (1 if state["failed"] else 0))


def main():
    import argparse
    import hashlib
    import pathlib
    import subprocess

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=pathlib.Path)
    parser.add_argument("library_dir", type=pathlib.Path)
    parser.add_argument("--mode", choices=("success", "failure", "ordered", "published", "notify", "isolation"),
                        required=True)
    args = parser.parse_args()
    for artifact in (args.elf, args.library_dir / "libcangjie-runtime.so",
                     args.library_dir / "libboundscheck.so"):
        with artifact.open("rb") as stream:
            digest = hashlib.sha256()
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        print("STARTUP_IDENTITY %s %s" % (digest.hexdigest(), artifact), flush=True)
    environment = os.environ.copy()
    environment["STARTUP_MODE"] = args.mode
    environment["LD_LIBRARY_PATH"] = str(args.library_dir.resolve())
    command = ["gdb", "-q", "-nx", "-batch", "-ex",
               "source " + str(pathlib.Path(__file__).resolve()), "--args",
               str(args.elf.resolve()), "--gtest_filter=" +
               ("RuntimeStartup.SubSchedulerIsolation" if args.mode == "isolation"
                else "PrecleanWithoutShutdown.WhiteBox")]
    try:
        result = subprocess.run(command, env=environment, timeout=45,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True)
        print(result.stdout, end="", flush=True)
        if args.mode == "failure" and result.returncode == 0:
            diagnostic = "Failed to start runtime scheduler: 11" in result.stdout
            print("STARTUP_TARGET FAILURE_DIAGNOSTIC verdict=%s error=11" %
                  ("PASS" if diagnostic else "FAIL"), flush=True)
            return 0 if diagnostic else 1
        return result.returncode
    except subprocess.TimeoutExpired as error:
        output = error.stdout or b""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        print(output, end="", flush=True)
        print("STARTUP_RESULT NOT_RUN debugger deadline exceeded", flush=True)
        return 124


if "gdb" in sys.modules:
    try:
        observe()
    except Exception as error:
        import gdb

        print("STARTUP_RESULT NOT_RUN %s" % error, flush=True)
        gdb.execute("quit 2")
elif __name__ == "__main__":
    sys.exit(main())
