# Final candidate red-entry preflight (read only)

Candidate `398d880bf` (resolved SHA in red-entry/*.json); required frozen base `8da2396b88608f0ab3ddbc65ddf7b05a414abc0c`. Product/tool files unchanged. Proposed cut files were generated entirely from git-show strings, never applied or built. Checker actually executed for six cases; runtime red results below are predictions, not test claims.

## Mechanical rule

`/root/cj_build/tools/entry_cut_check.py:main` reads `git show base:path` for exactly the candidate path; no rename detection or cross-file lineage. It runs `git diff -U0 base head -- path`, pairs removed/added **trimmed** text within that one file using a Counter to exempt moved lines. Remaining added positions are ineligible. Product removed lines must match baseline after `.strip()` (despite description saying verbatim, indentation alone is ignored). Function/class name changes on a one-line wrapper therefore matter. Phase recognition uses candidate AST-like brace spans / actual calls, not baseline function identity. At least one cut must hit PHASE_ENTRIES; passing baseline membership does not automatically supply phase eligibility.

## Exact probes and predicted target effect

| Proposed cut | Predicted target result if applied | Actual checker rc / reason |
|---|---|---|
| zGeneration.cpp:1115 remove `Heap::GetHeap().cross_vm().ProcessExportRoots(oldMarkForeignRoots);` | Full-driver test still reaches PostTrace observation; before callback should print producer_carrier=0 then fail test_young_weak.cpp:868. Actual runtime must confirm no earlier failure. | 1: same-file baseline instead calls `ProcessExportRoots(foreignRootsSet);` at :970; current line new. mark_end not in current phase list. |
| zRelocationSet.cpp:66 remove `Heap::GetHeap().cross_vm().PrepareCycleRef();` | Before callback should remain valid; after callback prints handoff_current=0 and fails :876 before later preforward CHECK can mask it. | 1: phase PostTrace recognized, but baseline :67 is unqualified `PrepareCycleRef();`; changed text/added position rejected. |
| zCrossVM.cpp:272 remove `destination.splice(destination.end(), entry.second);` | Product handoff publishes owner key but not edge pair, so after callback should fail handoffCurrent. | 1: zCrossVM.cpp absent from frozen baseline, so moved implementation cannot meet same-path rule; also PrepareCycleRef not phase entry. |
| zCollectedHeap.cpp:188 remove `collected->_runtime_workers.stop();` | RuntimeWorkers.HeapStopJoinsRuntimePool should pass initial nonzero pool checks and fail created_workers()==0 after Heap::StopGCWork; code path otherwise still stops GC services. | 1: baseline stop only routed to GetCollectorResources().StopGCWork(); new value-member shutdown call absent. `stop` not phase entry. |
| zHeap.cpp:416 replace full one-line StopGCWork body with empty body | Test should see existing worker counts after real Heap entry returns, fail created_workers()==0; omit actual stop but keep symbol definition. | 1: baseline same path has `void HeapImpl::StopGCWork() { ZCollectedHeap::stop(); }`, whole deleted line differs; StopGCWork not phase list. |

Full test location correction: exported owner callback/driver test is **test_young_weak.cpp:840-880,935**, not clear_entries_product_unit.cpp. Worker test is **test_gc_thread_pool.cpp:461-480**. Both use real product objects/functions; no copied helper is required for above predicted cuts. The worker test currently directly enters Heap::StopGCWork; it does not prove the CangjieRuntime finalizer caller executes.

## Positive control and boundary

Checker positive control: removing unchanged `space.GetRegionManager().HandleTraceRegions();` at zRelocationSet.cpp:60 is rc=0, PostTrace phase recognized. Baseline also contains exact call in same path. This confirms checker is running and can accept a genuine unchanged phase call; **it is not an ownership producer/consumer cut** and no red causality has been shown for chosen ownership test. Do not pair an irrelevant passing cut with an invalid ownership cut or claim control validates handoff.

The strongest relevant producer/consumer and worker cuts above are usable runtime experiments but are **not valid delivery entry cuts under the frozen-base checker**. The observed block is structural migration + same-path/text rule; no supported rename/line-tracking switch exists. Required action is advisor ruling for this concrete candidate: either explicitly authorize lineage-aware matching/registration, or a concrete base exception bound to candidate and before/after source anchors. Do not change baseline silently, alter entry list unilaterally, reintroduce old method names, or report an invalid cut as accepted. These findings do not prove every conceivable upstream cut impossible; they prove the direct mechanism cuts listed are rejected and identify why a different accepted cut would still need runtime causal evidence.

## Reproducible artifacts

`.lane-parallel/red-entry/{export-producer,export-consumer,crossvm-consumer,runtime-stop,heap-stop,checker-positive}.{diff,json,log}`. Each JSON binds full base/head SHA and records per-line eligibility and phase hits. index.json lists first five coordinates (all rc=1); checker-positive.json/log independently records rc=0. No builds/test runs occurred here.
