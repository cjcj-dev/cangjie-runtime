# Export-owner reachability (B10 / #534)

Coordinates: baseline `7e95511ac96ea63354cac97789b827b68986e11f`.
This repository document records the implementation; any coordination registration belongs to the controller.

| Reference implementation | Invariant | Cangjie implementation | Relationship |
|---|---|---|---|
| ZGC `zMark.cpp:392-435`, especially `:412-415` | The global mark bitmap suppresses repeated GC marking and following. | `runtime/src/Heap/z/zMark.cpp`, `ConcurrentMarkingWork::ProcessEntry` and `RunMajorStripeMark` | Equivalent GC deduplication; its result says nothing about a particular export owner. |
| ZGC `zReferenceProcessor.cpp:175-203,261-282` | Reference discovery is separate from retaining a referent strongly. | `HeapIterator::Fields(object, false, ...)` in `TracingCollector::ProcessExportRoots` | Strong ownership edges exclude the weak referent. Discovery and GC liveness remain unchanged. |
| No JNI equivalent for cross-language cycle ownership | Each export owner must retain every reachable foreign carrier exactly once, even if another owner or an earlier GC phase has marked it. | `TracingCollector::ProcessExportRoots` uses one visited set per owner and deduplicates multiple handles naming the same owner. | Language infrastructure difference, deliberately retained. |

The ordinary root closure finishes before `TryEndOldMark` calls `ProcessExportRoots` (`runtime/src/Heap/z/zGeneration.cpp:693-710`). For each export owner, the existing GC marking closure still runs, followed by a separate strong-edge DFS. Neither owner admission nor that DFS depends on `IsMarkedObject` or `MarkEntryObject` returning a first mark. The per-owner visited set bounds cycles and shared paths; it is discarded after that owner completes. Temporary space is proportional to that owner's graph, and total work is the sum of owner-reachable graphs.

The produced `discoveredExternObjects` map flows through `FindUselessExternObjects` currentization and `WCollector::PostTrace` → `PrepareCycleRef` into `cycleRefWorkStack`. No value is injected into those maps by the tests. The GC mark counters and live-byte accounting remain owned by the GC marker.

Tests retain `ValueRootCurrentization.MajorProducerConsumerCurrentizesBeforeMark` and its producer/handoff assertions. `ValueRootCurrentization.MajorExportOwnersSharePremarkedCycle` adds two owners sharing a cyclic graph and duplicate handles for one owner; each owner must have exactly one foreign carrier at production and handoff. Other ValueRootCurrentization mechanisms remain outside #534.
