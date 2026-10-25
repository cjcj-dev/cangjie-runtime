# Reference carrier migration on the latest runtime

Inputs: fork `259ad49f709222e75bdb832bcc28dc769ea4efbf`, canonical
`1a4c6aa41a804b318b2053e18b24b20d1dc3fcb7`, runtime donor
`0e6458945a7e5427565e576ecbb7bbe1370f4d04`, std donor
`0dbd8997109df82a88c06979b579a78616461089`.
The controller's runtime_gc_takeover_astra_1025-20261025T134600Z contract
authorizes replacing retired native registry tests while retaining their
behavioral obligations. This document records source migration, not acceptance.

ZGC `5b2d6991a1279d375f9a3c00c7bcd0bbcc7081d6`,
zReferenceProcessor.cpp:175–259, uses Reference fields and marking ownership;
it does not allocate a native registration node for each referent.
The managed Finalizer owns registration. One external pending root holds the
discovered chain until the managed handler transfers it.

| Retired test | Replacement obligation and consumer |
|---|---|
| FinalizerRelease.EnqueueMove | PendingTransferClearsPersistentRoot observes the cleared persistent root after transfer. |
| FinalizerRelease.EnqueueNull | ReferenceProcessor's inactive/null referent branch; null pending transfer must remain empty. |
| FinalizerRelease.EnqueueFiller | Old weak native registration slot no longer exists; no filler may enter the managed reference chain. Reference layout and real managed construction supply the legal input. |
| FinalizerRelease.WorkerFinalized | reference_carrier_product.cj checks finalizer_once, finalizable_child and finalizer_not_repeated. |
| FinalizerRelease.WorkerNull | Handler terminates at an empty pending chain; native pending transfer and managed lifecycle consumers. |
| FinalizerRelease.WorkerFiller | Native worker slot has been removed; actual managed Finalizer and referent type must govern invocation. Managed lifecycle remains required. |
| FnlzRoots.RegistrationPreservesSlotAndYoungEpoch | Registration now creates a managed FinalReference. MarkDiscovery1036 and actual managed constructor/collection must preserve its fields across epochs. |
| FnlzRoots.RegisteredFinalizerIsRawPointerButNotStrongRoot | FINAL carrier retains finalizable, rather than strong, referent liveness in ReferenceProcessor and MarkDiscovery1036. |
| FnlzRoots.VisitFinalizersCountMatchesRegister | Managed registration and exactly-once finalization, observed by reference_carrier_product.cj. |
| FnlzRoots.RegistryMissDoesNotCountAsFinalEnqueue | No native lookup exists; ReferenceProcessor counts actual discovered/inactivated carriers. |
| FnlzRoots.RegisteredFinalizerMovesAndCountsExactlyOnce | FinalDiscoveryProcessEnqueue and managed finalizer_once/finalizer_not_repeated. |
| FnlzRoots.EnqueueBetweenIdleCheckAndCommitKeepsJobVisible | HeapMonitor protects pending publication and transfer; concurrent pending-list component and actual managed handler wakeup remain required. |
| FnlzRoots.SharedBlockHandlesSurviveRegistrationGrowth | Managed registration chain replaces per-object native handles. ExportBlockGrowthKeepsSlotsAndReleaseSkipsVacancies retains native storage growth coverage; managed registration growth is not yet qualified. |
| WeakRootsProduct.CollectionReportsDeadToOwnerOnce | Finalizer weak native storage is retired. StringDedup retains its own weak-owner cleanup assertions; Reference pending/managed cleanup has separate consumers. |
| RootStorageSegments.Strong / WeakFinalizer | PendingRoot checks the one persistent slot. Export retains multi-block coverage. |
| ReferenceProcessor.FinalDiscoveryIsClaimedOnce / DuplicateWeakPendingAcceptedOnce | Native Node CAS is retired. Discovery is entered once by object marking; MarkDiscovery1036 retains actual marking/strong-cycle controls. ConcurrentWorkersPublishOnePendingList checks distinct managed carriers. |

All replacements are NOT_RUN for this candidate until recorded execution.
They do not inherit the donor's acceptance. In particular, native components
cannot discharge registration, managed queue notification, finalizer invocation,
exception handling, shutdown or registration-growth obligations. Any remaining
uncovered obligation blocks full Reference qualification.

ReferenceProcessor tests now use four compiler-layout fields and the actual
product discovery, processing and enqueue functions. No test liveness/enqueue
callback substitutes for product behavior. Weak clearing is checked before
and after enqueue. FINAL retains the referent and sets next to self. The
replacement-referent test publishes a new strong referent between discovery
and processing; it no longer claims an injected mid-load race.

MarkDiscovery1036 enters the real old non-strong-reference phase after mark-end.
Its FINAL case roots a distinct carrier. Finalizable array accounting includes
that carrier's actual size. Existing safepoint fixture entry remains unchanged;
the donor's separate VM lifecycle rewrite is not copied.

RawNullExit.FinalizerAccept/Reject retain names and page/null assertions, but
enter ordinary allocation; these do not test constructor registration.
AllocationZeroing.FinalizerClearsOnlyObject becomes PinnedClearsOnlyObject,
retaining dirty backing, cleared object and untouched suffix assertions.
The reference-array allocation test no longer registers a non-finalizable array;
allocation, retirement, liveness and next-cycle assertions remain. Its removed
registration-publication observation is an outstanding managed-constructor
obligation, not a passing result.

run_standalone.sh changes only two processor import names. The full-symbol
negative check, main positive control and executable import requirement remain.
This two-line bash compatibility edit belongs to the cjcj#883 migration ledger.

The new source requires paired compiler publication of three methods and four
intrinsics, and two-argument NewPinnedObject lowering with constructor-return
registration. Old compiler/LLVM binaries are not a qualified consumer. Windows
export-contract regeneration from the new fixed compiler/LLVM source and real
DLL validation remain pending; the old export inventory is not re-labelled.
SOFT/PHANTOM remain explicitly unsupported in this donor/API, not qualified by
the WEAK/FINAL tests. Interpreter's two held ABI additions remain separate.
