# Syscall exit progress

This fixture uses product `RunCJTask`, `CJ_SyscallRead`, and `GetTaskRet`.
It links the runtime shared library; it does not compile scheduler sources.
Linux x86-64, GDB with Python, and Python pexpect are required.

Compile once against `runtime/src/Cangjie.h` and a default product library:

```sh
clang++ -std=c++17 -g -O0 -pthread -I runtime/src \
  runtime/tests/syscall_exit_progress/blocking_task.cpp \
  -L "$LIBRARY_DIR" -lcangjie-runtime -lboundscheck -o "$FIXTURE"
bash runtime/tests/syscall_exit_progress/run.sh "$RESULT_DIR" "$LIBRARY_DIR" "$FIXTURE"
```

Use the same fixture ELF for baseline, candidate, each independent cut, and
restoration. Each invocation copies its libraries and hashes them before use.
Run on kkk2 through the project box entry, with core dumps disabled and a CPU
window recorded. The five processes run concurrently in separate directories.

`asleep` holds the returning syscall on the actual no-P branch until the helper
has completed the empty final check and reached a futex wait. `gap` holds the
helper at ThreadStop before it can sleep. Both then let the publisher proceed
and release all threads. No debugger call wakes a processor. The no-P instruction
is resolved from the tested SO's ProcessorAlloc call and conditional branch,
so removing the enqueue call does not remove the ordering breakpoint.

`fast`, `slow-p`, and `ordinary` exercise the original-P return, idle-P slow
return, and ordinary nonblocking task respectively. Unexpected branch selection
is a harness error. The two ordered modes require every construction event
before the completion deadline begins. A missing event or breakpoint is a
harness error (rc=2), not a product assertion failure (rc=1).

The fast control holds monitor syscall preemption only after the ordinary task
has started, and stops at the successful product CAS branch. The slow-P control
stops on the actual successful allocation branch. Each then disables ordering
breakpoints and releases all threads before awaiting completion. This avoids
GDB breakpoint handling racing process exit. The ordinary control needs no
ordering breakpoints.

The target assertion reads the result of the product task future and the task's
actual read result. Its five-second deadline is identical across product arms.
Each PASS is printed by the fixture; qualified lack of completion is reported
by the runner at `ordinary_task_completion_after_pipe_release`. `_Exit` limits
the test to task progress, excluding runtime shutdown. Debugger snapshots and
CJThreadFree observations support path and lifetime analysis, not an independent
assertion of complete scheduler memory-model correctness.

For a separate lifetime observation, prepare a directory with the same
`blocking_task` and `lib/`, then run `python3 run.py "$RESULT_DIR" lifetime`.
This holds the publisher before ThreadStop until the consumer has returned
from CJThreadFree. Its return breakpoint uses the actual x86-64 call return
address, since the runtime's switched stacks need not support GDB frame-based
finish breakpoints. The ordinary five-case suite has no reclamation breakpoint.

`cut-enqueue.diff` disconnects the baseline publication call in SyscallExit0.
`cut-wake.diff` disconnects the candidate wake call. Each should affect only
the two ordered no-P cases. Apply each in its own product tree, build the SO,
and run this same fixture and runner. Restore the candidate product and rerun.
