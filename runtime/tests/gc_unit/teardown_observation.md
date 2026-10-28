# Runtime-worker teardown observation

`run_standalone.sh` requires both the complete case runner and
`run_other_vm_teardown.sh` to succeed. A successful case tally does not replace
the latter's worker-lifetime assertions.

The GDB arm stops at `Heap::StopGCWork` and `GcUnit::CompleteTestRun`. It must
observe a live runtime-worker pool at the first boundary, no live workers at
the second, published created/active counts of zero, and a normal product exit.
Reading these counts must not execute code in the inferior. The expression
follows the same members as `ZCollectedHeap::safepoint_workers()` and
`ZRuntimeWorkers::workers()`, then reads `WorkerThreads::_created_workers` and
`_active_workers`. `WorkerThreads::stop()` joins the workers before clearing
these counts.

The current Linux toolchain uses libstdc++; its atomic integer debug type
exposes `_M_i`. This observer reads that member through debug information, not
an assumed byte offset. An unavailable type/member is an observation error;
there is no zero-value fallback. A different standard-library representation
requires an explicit observer adaptation and validation. GDB's
`set may-call-functions off` rejects accidental inferior calls, including a
future reintroduction of the getter expressions. It does not disable the
breakpoints or normal program execution.

The other two processes preserve the existing classifier controls:

- `exited`: an unreaped zombie worker is accepted after the real join, rc 0.
- `live`: a stopped, still-live worker is rejected, target assertion FAIL and
  rc 1, while the product itself must subsequently exit 0.

`check_teardown_records.py` requires these distinct results, identity/order
records, and complete cleanup. A live-control rc 1 is expected; a GDB-arm rc 1
is not. No child result may be overwritten to make the aggregate pass.

For the correction prompted by run 40688545051, preserve the original logs
and products. The original debugger arm exited 1 after an extended-state
error while evaluating getter calls; its product exited 0, but no pool-count
sample was produced. This does not establish a runtime teardown defect or
the underlying debugger/kernel error's unique cause.

The original debugger owner must validate the changed observer against a
fixed real ELF and matching runtime/bounds libraries, retaining child and
aggregate results. A targeted cut that restores the getter expressions while
keeping calls forbidden must fail at the count observation, with the live and
exited controls unchanged; restoring the field expressions must recover.
This is an observer gate, not a new runtime ABI or a waiver of the full gate.
Neither source inspection nor this plan constitutes a dynamic PASS.

GDB's documented function-call permissions:
https://sourceware.org/gdb/current/onlinedocs/gdb.html/Calling.html
