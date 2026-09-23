# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
# Run by gdb -batch -x against the real cj_gc_unit executable. All-stop
# breakpoints freeze the observation boundary; no runtime instrumentation.
import pathlib
import gdb

live = []
completed = []
exit_codes = []
observation_errors = []


def runtime_workers():
    tasks = pathlib.Path("/proc") / str(gdb.selected_inferior().pid) / "task"
    return sorted(p.read_text().strip() for p in tasks.glob("*/comm")
                  if p.read_text().startswith("RuntimeWorker#"))


class Observe(gdb.Breakpoint):
    def __init__(self, symbol, observations):
        super().__init__(symbol, internal=True)
        self.observations = observations

    def stop(self):
        try:
            workers = runtime_workers()
            self.observations.append(workers)
            print("TEARDOWN_OBSERVE %s workers=%d names=%s" %
                  (self.location, len(workers), ",".join(workers)))
        except Exception as error:
            observation_errors.append(str(error))
        return False


gdb.execute("set pagination off")
gdb.execute("set confirm off")
gdb.execute("set breakpoint pending on")
Observe("MapleRuntime::Heap::StopGCWork()", live)
Observe("MapleRuntime::GcUnit::CompleteTestRun(int)", completed)
gdb.events.exited.connect(lambda event: exit_codes.append(getattr(event, "exit_code", None)))
gdb.execute("run")
positive = len(live) == 1 and len(live[0]) > 0
ordered = len(completed) == 1 and len(completed[0]) == 0
executed = exit_codes == [0] and not observation_errors
print("ASSERT_TEARDOWN_LIVE samples=%d %s" % (len(live), "PASS" if positive else "FAIL"))
print("ASSERT_TEARDOWN_BEFORE_SENTINEL samples=%d %s" %
      (len(completed), "PASS" if ordered else "FAIL"))
print("ASSERT_TEARDOWN_EXECUTED exits=%s errors=%s %s" %
      (exit_codes, observation_errors, "PASS" if executed else "FAIL"))
gdb.execute("quit %d" % (0 if positive and ordered and executed else 1))
