# Thread barrier lifecycle

ZGC `zBarrierSet.cpp:253-273` initializes GC state on attach and flushes it on
detach. `runtime/threads.cpp:1089-1104` runs the detach flush inside
`Threads::remove` while the exiting thread is not yet safepoint-safe, and
`safepoint.cpp:346/465` holds Threads_lock for the whole pause, so no pause
can overlap the final publication. Physical GC state destruction is a later
operation (`zBarrierSet.cpp:248-251`).

Cangjie `MutatorManager::TransitMutatorToExit` runs
`ZBarrierSet::on_thread_detach(gcData)` before `EnterSaferegion(false)`. STW
waits until every mutator is `InSaferegion`, so until the detach flush
finishes no pause can complete; this ordering is the Cangjie form of the
Threads_lock exclusion (the management write lock itself cannot be held
across the flush: STW holds it while waiting for this very thread to stop,
so acquiring it before `EnterSaferegion` would deadlock). List removal
(`ThreadsSMRSupport::remove_thread`, threads.cpp:1108-1114) runs under the
management write lock in `DestroyMutator`; the registry-reader wait remains
in `on_thread_destroy`/`smr_delete`, outside that lock.

`ZMark::HandshakeFlush` matches ZGC `try_end` (zMark.cpp:954-970): a stopped
world flushes only GC threads; mutator stacks are drained by the concurrent
handshake flush and by the exit-path detach flush. There is no per-owner
mutex in `do_thread` (zMark.cpp:535-557 has none); a saferegion-resident
owner never touches its marking state.

`test_thread_detach_gdb.py` uses the existing
`ThreadLifecycle.ManagedDetachMarksBothGenerations` fixture. It stops the
exit thread at the first real `MarkStripeStackList::Push` after
`TransitMutatorToExit` entry, reads the owner's saferegion word from the live
`TransitMutatorToExit` frame, and asserts the publication runs with
`SAFE_REGION_FALSE`. After the publication it asserts the shared list
contains exactly one reference to the actual stack.

The script changes scheduling and invokes existing product operations. It does
not replace functions, modify GC state, add hooks or use breakpoint hit counts
as results. Optimized tail calls may remove the `TransitMutatorToExit` frame;
the frame walk then raises a harness error rather than passing.

Run on kkk2 with matching source/ELF/SO, a claimed CPU range and a fresh output:

```sh
GC_UNIT_TEST_ELF=/absolute/path/cj_gc_unit \
GCV2_RUNTIME_LIB_DIR=/absolute/path/lib \
DETACH_CPUSET=96-111 DETACH_OUT=/absolute/path/receipt \
bash runtime/tests/gc_unit/run_thread_detach_gdb.sh
```

This is a deterministic correctness test of the managed-exit/publication
ordering, not a throughput measurement or a substitute for native-exit and
attach tests.
