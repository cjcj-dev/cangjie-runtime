# Thread barrier lifecycle

ZGC `zBarrierSet.cpp:253-273` initializes GC state on attach and flushes it on
detach. `runtime/threads.cpp:1090-1114` keeps the detach hook in the thread-removal
critical section; physical GC state destruction is a later operation
(`zBarrierSet.cpp:248-251`).

Cangjie `Mutator::ResetMutator` runs after the last watermark/safe-region
transition, with `MutatorLock` held. `ZMark::HandshakeFlush` takes this same lock
when flushing an inventory owner. Therefore `on_thread_detach(gcData)` must
finish before `MutatorUnlock`. The registry-reader wait remains in
`on_thread_destroy`, after detach and outside that lock.

`test_thread_detach_gdb.py` uses the existing
`ThreadLifecycle.ManagedDetachMarksBothGenerations` fixture. It saves the real
owner at `ResetMutator`, stops its first real `MarkStripeStackList::Push`, then
observes the owner's mutex from the joining thread using `pthread_mutex_trylock`.
When available, it releases the observation lock and calls the real
`ZMark::FlushAllGenerations` entry on that second thread before resuming the
pending publication. Otherwise it resumes to `UnbindMutator`, after reset has
released the lock, and runs the same GC consumer there. It asserts the shared
list contains exactly one reference to the actual stack. With unlock moved
before detach, the real GC consumer and the pending exit publish the same stack
twice. The failing case stops before consuming that duplicate list.

The script changes scheduling and invokes existing product operations. It does
not replace functions, modify GC state, add hooks or use breakpoint hit counts
as results. Optimized tail calls may remove the `ResetMutator` frame in the
negative arm, hence the owner is captured at entry. All product breakpoints are
in the runtime SO; the fixture's ordinary final mark assertions still execute
in the passing arm.

Run on kkk2 with matching source/ELF/SO, a claimed CPU range and a fresh output:

```sh
GC_UNIT_TEST_ELF=/absolute/path/cj_gc_unit \
GCV2_RUNTIME_LIB_DIR=/absolute/path/lib \
DETACH_CPUSET=96-111 DETACH_OUT=/absolute/path/receipt \
bash runtime/tests/gc_unit/run_thread_detach_gdb.sh
```

This is a deterministic correctness test of the managed-exit/inventory branch,
not a throughput measurement or a substitute for native-exit and attach tests.
