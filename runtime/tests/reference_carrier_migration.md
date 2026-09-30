# Reference carrier test migration

Coordinates: runtime baseline 583370740011dc5d3f465872549f6dacb0617af7.

The removed native per-referent registry, job list and worker-slot release have
no counterpart in ZGC zReferenceProcessor.cpp:175-259 and java.lang.ref.Finalizer.
Reference.discovered carries pending links and Finalizer carries registration.
The single external pending root remains; PendingTransferClearsPersistentRoot
and RootStorageSegments.PendingRoot observe that root. Export storage assertions
are preserved. These component tests do not claim managed finalization coverage;
the actual managed lifecycle test is reference_carrier_product.cj.

## Removed native registry/queue tests (disposition i, carrier package #1356)

- `FinalizerRelease.EnqueueMove`
- `FinalizerRelease.EnqueueNull`
- `FinalizerRelease.EnqueueFiller`
- `FinalizerRelease.WorkerFinalized`
- `FinalizerRelease.WorkerNull`
- `FinalizerRelease.WorkerFiller`
- `WeakRootsProduct.CollectionReportsDeadToOwnerOnce`
- `FnlzRoots.RegistrationPreservesSlotAndYoungEpoch`
- `FnlzRoots.RegisteredFinalizerIsRawPointerButNotStrongRoot`
- `FnlzRoots.VisitFinalizersCountMatchesRegister`
- `FnlzRoots.RegistryMissDoesNotCountAsFinalEnqueue`
- `FnlzRoots.RegisteredFinalizerMovesAndCountsExactlyOnce`
- `FnlzRoots.EnqueueBetweenIdleCheckAndCommitKeepsJobVisible`
- `FnlzRoots.SharedBlockHandlesSurviveRegistrationGrowth`
- `RootStorageSegments.Strong`
- `RootStorageSegments.WeakFinalizer`

RootStorageSegments.Strong is replaced by PendingRoot: there is now exactly one
persistent external pending slot, so a multi-block finalizer-slot test has no
product input. RootStorageSegments.Export retains multi-block coverage.

## Existing behavior moved to actual Reference payloads

- ReferenceProcessor cases: four compiler-layout fields; product discovery,
  process_references and enqueue_references; consume the actual pending root.
  No liveness/enqueue callback is injected. Strong liveness rejects discovery
  itself (zReferenceProcessor.cpp:175-195), and FINAL sets next to self while
  retaining its referent (same file:222-230).
- FinalDiscoveryIsClaimedOnce / DuplicateWeakPendingAcceptedOnce: remove native
  Node duplicate-claim tests. ZGC discovery is entered once by object marking;
  it has no per-Node CAS (same file:237-259). Mark-entry tests retain actual
  strong-cycle/no-discovery observations.
- ProcessConsumerPreservesReplacementReferent: replacement is published into the
  actual referent slot between discovery and processing, replacing the removed
  test liveness callback. It does not claim the old callback-injected race.
- MarkDiscovery1036: finalizable graph is reached from a distinct rooted FINAL
  carrier. The weak/strong/finalizable/young and multi-worker assertions remain.
- FinalizableArrayClosureAccountsWithoutStrongUpgrade: adds the actual FINAL
  carrier root and accounts for that additional object and its exact size;
  finalizable array/child liveness assertions remain unchanged.
- run_standalone product-import guard uses the actual renamed processor symbols;
  full-symbol negative check and executable import requirements remain enabled.

Managed lifecycle coverage is blocked by separately assigned runtime#1399:
no finalizer target assertion has executed successfully. Native registry test
retirement is not represented as lifecycle acceptance.

The weak clear is observed both before enqueue and after pending publication;
StrongUpgrade preserves the finalizable-to-strong bitmap assertion on the
referent. ConcurrentWorkersPublishOnePendingList keeps 32 distinct FINAL
carriers/referents across eight producers and the two-worker product processor.

Fixture corrections found during the first real run:
- ReferenceProcessor and pending-transfer component tests attach as native GC
  threads before invoking barriers. No CJThread exists in that fixture; the old
  callbacks had concealed the native-to-scheduler lookup. Assertions unchanged.
- StoreAccess843/AccessBarrier976 weak TypeInfo inputs now contain the compiler
  field-offset table. The prior weak-kind-only inputs cannot describe the ABI.
- StoreAccess1085 remains a valid headerless-record invariant. The product
  is_referent_field must reject a non-referent offset before dereferencing a
  class, matching javaClasses.cpp:3954-3960; this is a product correction.

WeakGraph (YoungWeakClosure/MarkingStacksProduct/HeapIterator) now reserves and
initializes all four reference fields with a four-bit GCTib and offset metadata.
All graph, reachability, edge-visitor and liveness assertions are retained.

## Allocation entry migration (controller 20260930T043333Z)

| Test | Before | After | Preserved input / assertion |
|---|---|---|---|
| RawNullExit.FinalizerAccept | MCC_NewFinalizer | MCC_NewObject | same page, allows_raw_null, normal return |
| RawNullExit.FinalizerReject | MCC_NewFinalizer | MCC_NewObject | page set_is_relocate_promoted, exact raw-null diagnostic |
| AllocationZeroing.FinalizerClearsOnlyObject | MCC_NewFinalizer | MCC_NewObject | dirty reused backing, zero object, untouched suffix |

The test names are retained for the migration ledger; these observe ordinary
allocation, not finalizer registration. Dedicated allocation is removed by #1394
and constructor-completion registration belongs to #754. HotSpot
instanceKlass.cpp:1935-1938 uses ordinary allocation. No fake callback or missing
core bypass is installed. Registration coverage remains explicitly incomplete.

Disposition ii: YoungWeakClosure major and MarkingStacksProduct major expect
referent address null after cleaning, not raw bits zero. zBarrier.inline.hpp:554-560
uses color_mark_good when healing the weak field. Inspect is_null_any and print
the actual field bits; all reachability and cleared-address assertions remain.

Merge f29281826c: preserve main WorkerBudgetFixture{2} + ZWorkers(old, stats)
in BoundRefProc. Other conflict hunks only changed worker construction inside
retired native-registry tests listed above; keep their authorized retirement.
All main marking cache/termination and FollowWork lifetime changes retained.
