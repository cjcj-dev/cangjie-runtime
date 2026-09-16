# P08 producer → consumer migration ledger (WIP)

Coordinates: runtime `91f3dcc232201d4ad98ec3af6f10165edb950416` (frozen task base `b38fcfdef926f6ae5dff2e8d0ffa8f4d6356bc18`); ZGC `/root/cj_build/reference/jdk/src/hotspot/share/gc/z/`.
This is an implementation ledger, not an independent review or completed acceptance claim.

| Producer / runtime entry | Consumer in current product | Change boundary / ZGC anchor |
|---|---|---|
| MObject::Store / MArray::Store templates, ObjectModel/MObject.inline.h:65 / MArray.inline.h:82 | Barrier::WriteField → WriteI8..F64, zBarrier.inline.hpp:72 / zBarrier.cpp:135 | Primitive writes use Field::SetFieldValue directly; ZBarrierSet::barrier_needed false for primitives, zBarrierSet.cpp:230–244. No reference semantics change. |
| MCC_NewCJThread / NewExclusiveCJThread / NewCJThreadNoReturn, CjScheduler.cpp:382,507,645 | WritePlainRoot → StorePlain; task wrappers read at :359,487,624 | Plain mutator roots are direct loads/stores; GC root processing stays separate, ZUncoloredRoot.inline.hpp:34–67; PLAN I3/I4/I6. |
| Sync MCC_SetCurrentCJThreadObject, Sync.cpp:666 | MRT_GetCurrentCJThreadObject, :650 | Same direct plain-slot access; retain existing TSAN synchronization. |
| VisitStrongPlainRoots, zMark.cpp:689 / zHeapIterator.cpp:87 | ReadPlainRoot, :690 / :89 | Preserve root visitors and callbacks; remove only wrapper at mutator/plain-access boundary. |
| CJ_MCC_ReadRefField, CompilerCalls.cpp:1868 | Native / heap / plain routes, :1870–1876 | Plain branch loses wrapper now; static routing/preloaded ABI requires joint compiler migration later. |
| WriteReference / atomic entries, zBarrier.cpp:186 / :372 | StoreBarrier / RecordCrossGenEdge → TLS buffer.Add, :151 / :779 / :790 | Unified static funnel before slow path; migrate buffer producer and consumer together. |
| MutatorManager mark-flush handshakes, MutatorManager.cpp:693 / :713 | StoreBarrierBuffer::Flush / MarkAndRemember, zStoreBarrierBuffer.cpp:54 | Separate on_new_phase from flush; preserve #607 remset chain before replacing consumer. |
| mark array/object scan, zMark.cpp:327 / :372 / :389 | TraceRefField :192 → duplicated resolve/heal | Consume #607 final field/root split and generation semantics; no whole-file replacement. |

Mechanical source inventory: `baseline-consumers.json`, each query includes rc and full matching lines. Further compiler/TLS/relocation consumers remain to enumerate before edits to those faces.

## Pending rulings
- Literal phase-zero invariant conflicts with ZGC keep_alive_young (zBarrier.cpp:61) and buffer is_old_mark (zStoreBarrierBuffer.cpp:190,217); advisor asked.
- LLVM frozen coordinates, capacity receipt, and #607 shared-function meeting coordinate requested.

## 202355Z accepted target-owner seam (implementation in progress)

ZGC head ordering: zStackWatermark.cpp:177–206. Current roots must use saved target color before masks/buffer advance; the earlier proposed pre-root update is rejected.

- Mutator::Init (Mutator.h:85) creates/registers its GC data; BindMutator / ThreadLocalData::SetMutator (MutatorManager.cpp:105, ThreadLocal.cpp:24) only rebind the borrowed OS pointer. GC/native worker entry and cleaner own a separate native data object.
- Mutator lifecycle exit (MutatorManager.cpp:151) publishes target buffer/stacks before unbinding; cleaner destroys only native-owned data. Inventory moves from OS bindings to live data identities, protecting parked/unbound owners.
- Explicit-target data flush/verify in zMark.cpp:1363 and MutatorManager.cpp:728 differs from current-executor production in zMark.cpp:1356. Remote scanners must not rebind themselves to the target.
- ZGlobalsPointers::set_good_masks (zAddress.cpp:59) supplies one published attach snapshot. Mutator::DrainStackWatermark successful claims (Mutator.cpp:956,979), born-clean/epoch0/REMAP fallback and PreparedToRun/Park recovery must all reach the same phase-front contract before compiled accesses.
