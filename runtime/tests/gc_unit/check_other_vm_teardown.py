# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
# Run by gdb -batch -x against the real cj_gc_unit executable. All-stop
# breakpoints freeze the observation boundary; no runtime instrumentation.
import pathlib


def runtime_workers(pid):
    tasks = pathlib.Path("/proc") / str(pid) / "task"
    workers = []
    for path in tasks.glob("*/stat"):
        try:
            record = path.read_text()
        except FileNotFoundError:
            continue
        name = record[record.index("(") + 1:record.rindex(")")]
        state = record[record.rindex(")") + 2:].split()[0]
        if name.startswith("RuntimeWorker#"):
            print("TEARDOWN_WORKER_STATE tid=%s name=%s state=%s" %
                  (path.parent.name, name, state), flush=True)
        if name.startswith("RuntimeWorker#") and state not in ("Z", "X"):
            workers.append(name)
    return sorted(workers)


def teardown_before_sentinel(completed):
    ordered = len(completed) == 1 and len(completed[0]) == 0
    print("ASSERT_TEARDOWN_BEFORE_SENTINEL samples=%d %s" %
          (len(completed), "PASS" if ordered else "FAIL"))
    return ordered


def main():
    import gdb

    live = []
    completed = []
    published = []
    exit_codes = []
    observation_errors = []

    class Observe(gdb.Breakpoint):
        def __init__(self, symbol, observations, completion=False):
            super().__init__(symbol, internal=True)
            self.observations = observations
            self.completion = completion

        def stop(self):
            try:
                workers = runtime_workers(gdb.selected_inferior().pid)
                self.observations.append(workers)
                print("TEARDOWN_OBSERVE %s workers=%d names=%s" %
                      (self.location, len(workers), ",".join(workers)))
                if self.completion:
                    pool = "MapleRuntime::ZCollectedHeap::_collected_heap->safepoint_workers()"
                    counts = tuple(int(gdb.parse_and_eval("%s->%s()" % (pool, method)))
                                   for method in ("created_workers", "active_workers"))
                    published.append(counts)
                    print("TEARDOWN_POOL created=%d active=%d" % counts)
            except Exception as error:
                observation_errors.append(str(error))
            return False

    gdb.execute("set pagination off")
    gdb.execute("set confirm off")
    gdb.execute("set breakpoint pending on")
    Observe("MapleRuntime::Heap::StopGCWork()", live)
    Observe("MapleRuntime::GcUnit::CompleteTestRun(int)", completed, completion=True)
    gdb.events.exited.connect(lambda event: exit_codes.append(getattr(event, "exit_code", None)))
    gdb.execute("run")
    positive = len(live) == 1 and len(live[0]) > 0
    ordered = teardown_before_sentinel(completed)
    stopped = published == [(0, 0)]
    executed = exit_codes == [0] and not observation_errors
    print("ASSERT_TEARDOWN_LIVE samples=%d %s" % (len(live), "PASS" if positive else "FAIL"))
    print("ASSERT_TEARDOWN_POOL_STOPPED samples=%d %s" %
          (len(published), "PASS" if stopped else "FAIL"))
    print("ASSERT_TEARDOWN_EXECUTED exits=%s errors=%s %s" %
          (exit_codes, observation_errors, "PASS" if executed else "FAIL"))
    gdb.execute("quit %d" % (0 if positive and ordered and stopped and executed else 1))


if __name__ == "__main__":
    main()
