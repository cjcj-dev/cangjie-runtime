待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`。

# GC lifecycle schema (#1309)

坐标基于 `6f75e652e7025a50235670e9f9e00a8e2c6f10ed`。

Collection: `[GCLOG] v=6 rec=cycle seq=<id> gc_tag=- name=Minor_Collection|Major_Collection cause=<cause token> event=start|abort`.
End instead has `event=end start_ns=<u64> dur_ns=<u64> used_at_start=<bytes> used_at_end=<bytes>`.
Generation: `[GCLOG] v=6 rec=generation seq=<id> gc_tag=y|Y|O name=<name token> event=start|abort`.
End instead has `event=end start_ns=<u64> dur_ns=<u64> used_at_collection_start=<bytes> used_at_collection_end=<bytes>`.
Phase and STW retain baseline v5 fields and nanoseconds. Phase kind now includes `critical`; there is no `unknown` or phase_leaf producer.

`parse_gclog` does exact version/order/field/range validation. Streaming readers deliberately see starts before a terminal exists. Completed-run measurement readers call `validate_complete()`; it checks collection/generation lifecycle, generation-contained required collect phases, and reports `aborted` and `truncated` populations separately. Old ZSTAT and v4-cycle readers are removed, not retained behind switches.

# Producer and consumer dispositions

Raw mechanical base inventory: 1309-consumers-base.txt (95 lines).
- Base/GcLog.h and zStat.cpp: rewritten lifecycle records at ZGC zStat.cpp:654-741 routing points.
- zDriver minor/major: own used_at_start, matching ZGC zDriver.hpp:80-131; setter/getter selection in ZStatPhaseCollection (zStat.cpp:640-652).
- zPageAllocator::StallAllocation, zRelocateQueue::add_and_wait, zJNICritical::enter_inner: timer scopes at ZGC zPageAllocator.cpp:1436, zRelocate.cpp:134, zJNICritical.cpp:106.
- zReferenceProcessor: only two timer instances/calls removed; finalizer mechanism, assertions and comments remain. The `finalizerProcessor` matches in zCollectedHeap.cpp and headers are mechanism names, not timers.
- gclog_schema.py: exact schemas and completed-ledger validation; obsolete branches/tests removed.
- analyze_stw.py and analyze_drainwall.py: validate before aggregation; use only end durations; no missing-as-zero path.
- phase_entry_guard.py: raw reader with its existing independent entry assertions; count end records only.
- wait_phase_entry_cycle.py: streaming input; acknowledge only an end event, never start/abort.
- analyze_hapillar.py and analyze_youngstw.py: only consume phase/STW values via raw reader; no lifecycle fields affected. Old youngstw phase metric is explicitly excluded by task §3 C8.
- test_gclog_schema.py and test_phase_entry_guard.py: fixtures migrated, retain existing valid ns/range/order/tag/entry assertions.
- check_driver_receipt_wiring.sh: removed; it names a removed CopyCollector product function and has no callers.
- zStat.hpp/MutatorManager.h comments: no executable consumer; no change to counter/stat APIs outside this issue.
- Other inventory hits in .md/.sh are documentation or selectors using phase/STW fields that remain unchanged; individually classified in final inventory companion.
- cjcj release-gates.mjs: parse exact v5 phase ns, identify missing floor names explicitly as UNKNOWN. Controller prohibits floor changes; cjcj#748 owns that specification migration.

# Test changes

Removed obsolete phase_leaf path/depth/overflow and ZSTAT tests with their nonexistent producers (ZGC has no counterpart); replaced v4 family fixture with current lifecycle fixture. Added A1–A5/A7, count variation, generation isolation, duplicate terminal tests. A6 uses real product stderr. Added GcLifecycleLog.CollectionStart/CollectionEnd/CollectionAbort/GenerationUsed and testable AllocationStall; all run real driver or alloc_page entry and read stderr emitted by the linked SO.
