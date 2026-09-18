# Final HeapGcState owner fold map (read only)
待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`（若登记）。
Coordinates: active candidate worktree, HEAD 43ca9e13be1390ab35dbfd89ae909bd91b8dc96f. Below includes current unstaged owner migrations.

## State conclusion

HeapGcState instance fields are empty by complete class inspection (`zMark.hpp`, entire class reproduced as method inventory below). Remaining members are methods, RefSlotKind enum, four private aliases, friends and **seven test-only static std::function hooks**; no nonstatic mutex/container/counter/pointer data remains. Default constructor/destructor do no work. This means owner migration can remove the object, but **this still carries diagnostic provenance and helper receiver identity**, so cannot indiscriminately replace this with nullptr.

Static hooks: testColoredRootResult, testCyclePrepared, testYoungMarkStarted, testOldMarkStarted, testMarkStartState, testYoungMarkCompleted, testOldMarkThreadResult. Move phase hooks to ZGeneration / specific generation; root result hooks to actual ZMark root operation. Keep MRT_TESTABLE_INTERNALS gates and single zTracing.cpp definitions (or new owner cpp); do not duplicate hooks per collector replacement.

## Complete method inventory

Repeated rows are real overloads. Proposed owner is mechanism placement, not permission to bulk rename a collector to another facade. Existing roots header has no class named ZRootsIterator: it has RootsIteratorStrongColored/WeakColored/AllColored/StrongUncolored, JavaThreadsIterator, StaticRootsAdapterIterator; choose actual iterator or existing ZMark root orchestration, do not invent a generic collector-like ZRootsIterator bucket.

| Method | current line | proposed owner | exact declaration start |
|---|---|---|---|
| HeapGcState | zMark.hpp:333 | delete/direct consumer | `HeapGcState() = default;` |
| ~HeapGcState | zMark.hpp:335 | delete/direct consumer | `~HeapGcState() = default;` |
| MajorMark | zMark.hpp:336 | delete/direct consumer | `ZMark* MajorMark() { return Heap::GetHeap().old().MarkPtr(); }` |
| MajorMark | zMark.hpp:337 | delete/direct consumer | `const ZMark* MajorMark() const { return Heap::GetHeap().old().MarkPtr(); }` |
| PreGarbageCollection | zMark.hpp:338 | ZGeneration / exact young-old phase owner | `void PreGarbageCollection(ZGenerationId generation, bool isConcurrent, uint64_t gcIndex);` |
| PostGarbageCollection | zMark.hpp:339 | ZGeneration / exact young-old phase owner | `void PostGarbageCollection(ZGenerationId generation, uint64_t gcIndex);` |
| VisitStrongPlainRoots | zMark.hpp:357 | Roots iterators / ZMark mark-roots orchestration | `void VisitStrongPlainRoots(const RootVisitor& visitor,` |
| DumpRoots | zMark.hpp:361 | Heap / Heap allocator facade | `void DumpRoots(LogType logType);` |
| DumpHeap | zMark.hpp:362 | Heap / Heap allocator facade | `void DumpHeap(const CString& tag);` |
| DumpBeforeGC | zMark.hpp:363 | Heap / Heap allocator facade | `void DumpBeforeGC();` |
| DumpAfterGC | zMark.hpp:365 | Heap / Heap allocator facade | `void DumpAfterGC();` |
| DiscoverReference | zMark.hpp:373 | delete/direct consumer | `bool DiscoverReference(BaseObject* reference, ReferenceType type)` |
| DiscoverWeakReference | zMark.hpp:378 | ZMark (real mark/closure owner) | `void DiscoverWeakReference(BaseObject* reference, WorkStack& workStack);` |
| IsMarkedObject | zMark.hpp:383 | delete/direct consumer | `bool IsMarkedObject(const BaseObject* obj) const { return RegionSpace::IsMarkedObject<G>(obj); }` |
| IsSurvivedObject | zMark.hpp:387 | delete/direct consumer | `inline bool IsSurvivedObject(const BaseObject* obj) const` |
| MarkOldObjectIfActive | zMark.hpp:392 | ZMark (real mark/closure owner) | `void MarkOldObjectIfActive(BaseObject* object, bool gcThread = false) const;` |
| IsResurrectedObject | zMark.hpp:396 | delete/direct consumer | `inline bool IsResurrectedObject(const BaseObject* obj) const { return RegionSpace::IsResurrectedObject(obj); }` |
| UpdateGCStats | zMark.hpp:405 | ZStat/GCStats | `void UpdateGCStats();` |
| ForwardFromSpace | zMark.hpp:409 | ZRelocate | `void ForwardFromSpace(ZGenerationId generation);` |
| RefineFromSpace | zMark.hpp:410 | ZRelocate | `void RefineFromSpace();` |
| NewWorkStack | zMark.hpp:414 | delete/direct consumer | `inline WorkStack NewWorkStack() const` |
| EnumAllCommonRoots | zMark.hpp:422 | ZMark (real mark/closure owner) | `void EnumAllCommonRoots(ZWorkers& workers);` |
| GetWorkers | zMark.hpp:423 | delete/direct consumer | `ZWorkers& GetWorkers(ZGenerationId generation) const` |
| EnumAllExportRoots | zMark.hpp:428 | Roots iterators / ZMark mark-roots orchestration | `void EnumAllExportRoots(RootSet& foreignRootsSet);` |
| DiscoverFinalizableRoot | zMark.hpp:431 | ZMark (real mark/closure owner) | `void DiscoverFinalizableRoot(NativeSlot& slot) const;` |
| MergeMutatorRoots | zMark.hpp:433 | Roots iterators / ZMark mark-roots orchestration | `void MergeMutatorRoots(WorkStack& workStack);` |
| DoEnumeration | zMark.hpp:434 | Roots iterators / ZMark mark-roots orchestration | `void DoEnumeration(WorkStack& workStack, WorkStack& foreignRootsSet);` |
| VisitStaticRoots | zMark.hpp:444 | Roots iterators / ZMark mark-roots orchestration | `void VisitStaticRoots(const NativeSlotVisitor& visitor) const;` |
| YoungMark | zMark.hpp:477 | delete/direct consumer | `ZMark* YoungMark() { return Heap::GetHeap().young().MarkPtr(); }` |
| YoungMark | zMark.hpp:478 | delete/direct consumer | `const ZMark* YoungMark() const { return Heap::GetHeap().young().MarkPtr(); }` |
| EnumRefFieldRoot | zMark.hpp:481 | Roots iterators / ZMark mark-roots orchestration | `void EnumRefFieldRoot(RefField<>& ref, RootSet& rootSet) const;` |
| GetAndTryTagObj | zMark.hpp:482 | ZBarrier (colored slots only) | `BaseObject* GetAndTryTagObj(RefSlotKind kind, BaseObject* obj, RefField<>& field);` |
| ForwardObject | zMark.hpp:483 | ZRelocate | `BaseObject* ForwardObject(BaseObject* fromVersion, Generation generation);` |
| ForwardObjectExclusive | zMark.hpp:484 | ZRelocate | `BaseObject* ForwardObjectExclusive(BaseObject* obj);` |
| AddRawPointerObject | zMark.hpp:512 | Heap / Heap allocator facade | `void AddRawPointerObject(BaseObject* obj)` |
| PinRawPointerObject | zMark.hpp:522 | Heap / Heap allocator facade | `BaseObject* PinRawPointerObject(BaseObject* obj)` |
| RemoveRawPointerObject | zMark.hpp:549 | Heap / Heap allocator facade | `void RemoveRawPointerObject(BaseObject* obj)` |
| IsFromObject | zMark.hpp:558 | ZRelocate | `bool IsFromObject(BaseObject* obj) const` |
| IsGhostFromObject | zMark.hpp:568 | delete/direct consumer | `bool IsGhostFromObject(BaseObject* obj) const` |
| IsUnmovableFromObject | zMark.hpp:573 | ZRelocate | `bool IsUnmovableFromObject(BaseObject* obj) const;` |
| TryUpdateRefField | zMark.hpp:604 | ZBarrier (colored slots only) | `bool TryUpdateRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const;` |
| RootSlotWriteback | zMark.hpp:650 | ZBarrier (colored slots only) | `RefField<> RootSlotWriteback(BaseObject* target, const RefField<>& /*slot*/) const` |
| CollectLargeGarbage | zMark.hpp:655 | ZGeneration / exact young-old phase owner | `void CollectLargeGarbage()` |
| CollectPinnedGarbage | zMark.hpp:665 | ZGeneration / exact young-old phase owner | `void CollectPinnedGarbage()` |
| CollectSmallSpace | zMark.hpp:674 | ZGeneration / exact young-old phase owner | `void CollectSmallSpace();` |
| DoGarbageCollection | zMark.hpp:676 | delete/direct consumer | `void DoGarbageCollection(ZGenerationId generation);` |
| ProcessFinalizers | zMark.hpp:677 | ZMark (real mark/closure owner) | `void ProcessFinalizers();` |
| CasInstallResolvedTarget | zMark.hpp:685 | ZBarrier (colored slots only) | `bool CasInstallResolvedTarget(RefField<>& field, MAddress expected, zaddress target,` |
| ResolveMinorReference | zMark.hpp:687 | ZRelocate | `BaseObject* ResolveMinorReference(RefField<>& field,` |
| ResolveMinorReference | zMark.hpp:689 | ZRelocate | `BaseObject* ResolveMinorReference(RootSlot& root,` |
| VisitMinorRootSlots | zMark.hpp:691 | Roots iterators / ZMark mark-roots orchestration | `void VisitMinorRootSlots(RootVisitor& rawRootVisitor, RootVisitor& invisibleRootVisitor,` |
| VisitMinorRoots | zMark.hpp:693 | Roots iterators / ZMark mark-roots orchestration | `void VisitMinorRoots(const std::function<void(BaseObject*)>& visitor,` |
| PushYoungObject | zMark.hpp:697 | ZMark (real mark/closure owner) | `void PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin = "unknown") const;` |
| PushYoungObject | zMark.hpp:698 | ZMark (real mark/closure owner) | `void PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin, bool finalizable) const;` |
| TraceYoungClosure | zMark.hpp:701 | ZMark (real mark/closure owner) | `void TraceYoungClosure(WorkStack& workStack, bool fullYoungScan,` |
| TraceYoungClosureStriped | zMark.hpp:705 | ZMark (real mark/closure owner) | `void TraceYoungClosureStriped(WorkStack& workStack, bool fullYoungScan,` |
| FollowYoungMark | zMark.hpp:710 | ZMark (real mark/closure owner) | `bool FollowYoungMark(WorkStack& workStack, bool fullYoungScan,` |
| TryEndYoungMark | zMark.hpp:716 | ZMark (real mark/closure owner) | `bool TryEndYoungMark(WorkStack& workStack, YoungConcWindowStats* windowStats = nullptr);` |
| FixMinorEvacuatedSlot | zMark.hpp:718 | ZRelocate | `bool FixMinorEvacuatedSlot(RefField<>& field, BaseObject* knownBase = nullptr,` |
| FixMinorEvacuatedSlot | zMark.hpp:720 | ZRelocate | `bool FixMinorEvacuatedSlot(RootSlot& root, const ScopedStopTheWorld* stw = nullptr) const;` |
| FixMinorEvacuatedSlot | zMark.hpp:721 | ZRelocate | `bool FixMinorEvacuatedSlot(DerivedSlot& derived, BaseObject* knownBase = nullptr,` |
| FixMinorRootSlots | zMark.hpp:723 | ZRelocate | `void FixMinorRootSlots(const ScopedStopTheWorld* stw = nullptr);` |
| EvacuateYoungRegions | zMark.hpp:727 | ZGeneration / exact young-old phase owner | `void EvacuateYoungRegions(const std::vector<BaseObject*>& reachableVec, const MinorSlotSet& rememberedSlots, bool refFixSlotsCoveredByReachable,` |
| DoYoungGarbageCollection | zMark.hpp:732 | delete/direct consumer | `void DoYoungGarbageCollection();` |
| TryUpdateRefFieldImpl | zMark.hpp:737 | ZBarrier (colored slots only) | `bool TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& ref, BaseObject*& oldRef, BaseObject*& newRef,` |
| PostTrace | zMark.hpp:739 | ZGeneration / exact young-old phase owner | `void PostTrace();` |
| RemapYoungRoots | zMark.hpp:743 | ZRelocate | `void RemapYoungRoots();` |
| Preforward | zMark.hpp:744 | ZRelocate | `bool Preforward();` |
| StartRelocationTasks | zMark.hpp:745 | ZRelocate | `void StartRelocationTasks(ZGenerationId generation);` |

## Explicit direct-deletion cuts

- MajorMark/YoungMark -> Heap::old()/young().MarkPtr; GetWorkers -> generation.Workers. NewWorkStack -> local WorkStack construction. No forwarding wrappers retained.
- DoYoungGarbageCollection body is only Heap::young().collect(); DoGarbageCollection routes young/old collect. Driver/Heap already has concrete generations, replace at that actual call point; do not migrate wrappers.
- DiscoverReference -> existing ZReferenceProcessor::DiscoverReference. IsMarkedObject/IsSurvivedObject/IsResurrectedObject are RegionSpace wrappers; route callers directly to exact existing predicate (preserve old-only resurrection condition).
- IsGhostFromObject is only IsFromObject wrapper. Remove and use true forwarding-table membership; don't keep alias.

## Nontrivial dependencies / this audit

1. PinRawPointerObject embeds `this` as ForwardingProvenance holder. Once method is actual Heap member, holder becomes stable Heap identity; preserve slot pointer &obj and same validation/routing order. AddRawPointerObject and RemoveRawPointerObject should consume same unique allocator, not retain GetCollector.
2. ResolveMinorReference(RootSlot), FixMinorEvacuatedSlot(RootSlot/DerivedSlot) embed this as raw-root holder; select actual owning generation relocate object as diagnostic owner (stable lifetime), never nullptr or pointer to temporary. Keep full provenance field offsets and source slots.
3. Relocation helpers `EnsureRouteDomainMembership(HeapGcState*, ...)`, `ForceRootRouteDomainWhileForwardable(HeapGcState*, ...)` only use collector to invoke methods. After target methods migrate, remove parameter and all const_cast<HeapGcState*>(this); do not reintroduce new collector type. `RemapPromotedField(HeapGcState&,...)` similarly inspect and remove unused receiver after rerouting.
4. Lambda captures: old class bodies in zMark.cpp VisitMinorRoots capture this for ResolveMinorReference; ProcessFinalizers captures this for IsMarkedObject. RootsIterator.cpp EnumAllExportRoots captures this for EnumRefFieldRoot. zTracing.cpp DumpRoots captures this for predicates. zRelocate.cpp lambdas capture this for helper calls/provenance. Convert only captures tied to removed class; real ZMark/ZGeneration/ZRelocate this captures own state and must stay.
5. Private aliases: MinorObjectSet, MinorRegionSet, MinorSlotSet, MinorInteriorBaseMap. Check whole-repo consumption; currently closure signatures use MinorSlotSet and relocation uses MinorInteriorBaseMap. Prefer canonical existing generation aliases or explicit unordered_set/map types in shared signatures, rather than moving all aliases into a collector-shaped namespace. Unused two aliases may be deleted after paired grep.
6. RefSlotKind enum carries strong/weak behavior for GetAndTryTagObj. Move with real colored barrier helper or replace with existing ReferenceStrength only after matching weak behavior; don't introduce integer/bool conflation at callers.
7. Friends elsewhere: ObjectModel/CurrentObjectRef.h, RefField.h two locations, zGeneration.hpp Young/Old, zRelocate.hpp. Remove obsolete HeapGcState friendship; add true owner only for still-required private constructor/access. Public typed slot constructors should avoid new broad friend where possible. Tests own gated access structs; move narrowly to each real owner, not all friends everywhere.
8. Mutator::ForwardLocalFinalizers(HeapGcState&) parameter is unnamed in definition and can be removed with declaration/callers. Check all references mechanically. zStoreBarrierBuffer.cpp, zHeapIterator.cpp, zVerify.cpp retain collector locals; replace each predicate/call with real owner then erase local.
9. RootSlotWriteback here has only RefField overload returning ZBarrier::GetAndTryTagRefField; replace direct at consumers, preserving distinction from real plain RootSlot ABI. Do not invent raw-root recoloring.
10. Pre/PostGarbageCollection has no object state but manipulates generation Begin/End/workers/stats order. Make it generation-owned orchestration; don't mix cleanup order with static utility migration. PostTrace belongs old relocation-set phase; EvacuateYoungRegions belongs young phase orchestration with real ScopedStopTheWorld ownership.
11. UpdateGCStats controls allocator async mode/threshold and stats. Real owner is GCStats/Heap policy (`zStat.cpp:659`), not merely a diagnostic counter: preserve ordering before/after reclamation.

## Safe parallel shape after signature freeze

A: mark/roots methods + hooks + affected zMark/zRootsIterator implementation. B: relocation/slot mechanics + narrow ZBarrier methods. C: Heap/gen/stat/driver consumers and deletion of collectorImpl/GetCollector. Tests can work in separate files concurrently once exact static/member signatures and aliases freeze. **zMark.hpp is shared across all**: one integrator owns all removal/signature edits; each agent provides patch/request for it. zGeneration.cpp similarly one owner to prevent overwrite. Final class deletion only after all consumer inventories reach no live old type refs; grep strings in negative lineage tests must be treated separately.

## Mechanical production references

Command `rg -n 'HeapGcState::|HeapGcState[&*]|friend class HeapGcState|collectorImpl|GetCollector\(' runtime/src`; rc=0. Includes declarations/comments; use actual bodies for final call classification.
```text
runtime/src/Mutator/Mutator.cpp:872:inline void Mutator::ForwardLocalFinalizers(HeapGcState&)
runtime/src/Mutator/Mutator.cpp:965:    ForwardLocalFinalizers(Heap::GetHeap().GetCollector());
runtime/src/Mutator/Mutator.h:311:    inline void ForwardLocalFinalizers(HeapGcState& collector);
runtime/src/ObjectModel/RefField.h:53:    // ⚠ 本函数不做读屏障；需要 load-good 的路径必须走 HeapGcState::make_load_good。
runtime/src/ObjectModel/RefField.h:169:    friend class HeapGcState;
runtime/src/ObjectModel/RefField.h:223:    friend class HeapGcState;
runtime/src/ObjectModel/CurrentObjectRef.h:67:    friend class HeapGcState;
runtime/src/Heap/z/zHeap.hpp:84:    HeapGcState& GetCollector();
runtime/src/Heap/z/zHeap.hpp:85:    const HeapGcState& GetCollector() const;
runtime/src/Heap/z/zHeap.hpp:287:    std::unique_ptr<HeapGcState> collectorImpl;
runtime/src/Heap/z/zRelocationSet.cpp:56:void HeapGcState::PostTrace()
runtime/src/Heap/z/zRelocationSet.cpp:85:void HeapGcState::CollectSmallSpace()
runtime/src/Heap/z/zGeneration.hpp:229:    friend class HeapGcState;
runtime/src/Heap/z/zGeneration.hpp:275:    friend class HeapGcState;
runtime/src/Heap/z/zMark.cpp:63:void HeapGcState::EnumRefFieldRoot(RefField<>& field, RootSet& rootSet) const
runtime/src/Heap/z/zMark.cpp:130:BaseObject* HeapGcState::GetAndTryTagObj(RefSlotKind kind, BaseObject* obj, RefField<>& field)
runtime/src/Heap/z/zMark.cpp:167:void HeapGcState::VisitMinorRootSlots(RootVisitor& rawRootVisitor, RootVisitor& invisibleRootVisitor,
runtime/src/Heap/z/zMark.cpp:213:void HeapGcState::DiscoverFinalizableRoot(NativeSlot& slot) const
runtime/src/Heap/z/zMark.cpp:227:void HeapGcState::DiscoverWeakReference(BaseObject* reference, WorkStack& workStack)
runtime/src/Heap/z/zMark.cpp:263:        if (HeapGcState::testOldMarkThreadResult) {
runtime/src/Heap/z/zMark.cpp:264:            HeapGcState::testOldMarkThreadResult(mutator);
runtime/src/Heap/z/zMark.cpp:285:            if (HeapGcState::testColoredRootResult) {
runtime/src/Heap/z/zMark.cpp:286:                HeapGcState::testColoredRootResult(ZGenerationId::old, &slot);
runtime/src/Heap/z/zMark.cpp:297:        if (HeapGcState::testColoredRootResult) {
runtime/src/Heap/z/zMark.cpp:298:            HeapGcState::testColoredRootResult(ZGenerationId::old, nullptr);
runtime/src/Heap/z/zMark.cpp:314:void HeapGcState::EnumAllCommonRoots(ZWorkers& workers)
runtime/src/Heap/z/zMark.cpp:349:            if (HeapGcState::testColoredRootResult) {
runtime/src/Heap/z/zMark.cpp:350:                HeapGcState::testColoredRootResult(ZGenerationId::young, &slot);
runtime/src/Heap/z/zMark.cpp:358:        if (HeapGcState::testColoredRootResult) {
runtime/src/Heap/z/zMark.cpp:359:            HeapGcState::testColoredRootResult(ZGenerationId::young, nullptr);
runtime/src/Heap/z/zMark.cpp:371:void HeapGcState::VisitMinorRoots(const std::function<void(BaseObject*)>& visitor,
runtime/src/Heap/z/zMark.cpp:395:void HeapGcState::PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin) const
runtime/src/Heap/z/zMark.cpp:400:void HeapGcState::PushYoungObject(BaseObject* object, WorkStack& workStack, const char* origin,
runtime/src/Heap/z/zMark.cpp:523:        "HeapGcState::ScrubMinorFreeTarget.unresolved", target, oldVal,
runtime/src/Heap/z/zMark.cpp:551:void HeapGcState::TraceYoungClosureStriped(WorkStack& workStack, bool fullYoungScan,
runtime/src/Heap/z/zMark.cpp:582:void HeapGcState::TraceYoungClosure(WorkStack& workStack, bool fullYoungScan,
runtime/src/Heap/z/zMark.cpp:611:bool HeapGcState::FollowYoungMark(WorkStack& workStack, bool fullYoungScan,
runtime/src/Heap/z/zMark.cpp:639:bool HeapGcState::TryEndYoungMark(WorkStack& workStack, YoungConcWindowStats* windowStats)
runtime/src/Heap/z/zMark.cpp:656:void HeapGcState::ProcessFinalizers()
runtime/src/Heap/z/zMark.cpp:703:        BaseObject* target = Heap::GetHeap().GetCollector().ResolveMinorReference(field);
runtime/src/Heap/z/zMark.cpp:800:void HeapGcState::MarkOldObjectIfActive(BaseObject* object, bool gcThread) const
runtime/src/Heap/z/zMark.cpp:1073:    auto& collector = static_cast<HeapGcState&>(Heap::GetHeap().GetCollector());
runtime/src/Heap/z/zRootsIterator.cpp:173:void HeapGcState::VisitStrongPlainRoots(
runtime/src/Heap/z/zRootsIterator.cpp:183:void HeapGcState::VisitStaticRoots(const NativeSlotVisitor& visitor) const
runtime/src/Heap/z/zRootsIterator.cpp:188:void HeapGcState::MergeMutatorRoots(WorkStack& workStack)
runtime/src/Heap/z/zRootsIterator.cpp:194:void HeapGcState::EnumAllExportRoots(RootSet &foreignRootsSet)
runtime/src/Heap/z/zRootsIterator.cpp:201:void HeapGcState::DoEnumeration(WorkStack& workStack, WorkStack& foreignRootsSet)
runtime/src/Heap/z/zTracing.cpp:27:std::function<void(ZGenerationId, NativeSlot*)> HeapGcState::testColoredRootResult;
runtime/src/Heap/z/zTracing.cpp:28:std::function<void()> HeapGcState::testCyclePrepared;
runtime/src/Heap/z/zTracing.cpp:29:std::function<void()> HeapGcState::testYoungMarkStarted;
runtime/src/Heap/z/zTracing.cpp:30:std::function<void()> HeapGcState::testOldMarkStarted;
runtime/src/Heap/z/zTracing.cpp:31:std::function<void(ZGenerationId, MarkStartPoint, const ZMark*)> HeapGcState::testMarkStartState;
runtime/src/Heap/z/zTracing.cpp:32:std::function<void()> HeapGcState::testYoungMarkCompleted;
runtime/src/Heap/z/zTracing.cpp:33:std::function<void(Mutator&)> HeapGcState::testOldMarkThreadResult;
runtime/src/Heap/z/zTracing.cpp:114:void HeapGcState::DumpHeap(const CString& tag)
runtime/src/Heap/z/zTracing.cpp:144:void HeapGcState::DumpRoots(LogType logType)
runtime/src/Heap/z/zTracing.cpp:275:void HeapGcState::DumpBeforeGC()
runtime/src/Heap/z/zTracing.cpp:287:void HeapGcState::DumpAfterGC()
runtime/src/Heap/z/zStoreBarrierBuffer.cpp:160:    HeapGcState& collector = Heap::GetHeap().GetCollector();
runtime/src/Heap/z/zVerify.cpp:115:    auto& collector = static_cast<HeapGcState&>(Heap::GetHeap().GetCollector());
runtime/src/Heap/z/zVerify.cpp:127:    auto& collector = static_cast<HeapGcState&>(Heap::GetHeap().GetCollector());
runtime/src/Heap/z/zVerify.cpp:163:    auto& collector = Heap::GetHeap().GetCollector();
runtime/src/Heap/z/zRelocate.cpp:120:bool HeapGcState::IsUnmovableFromObject(BaseObject* obj) const
runtime/src/Heap/z/zRelocate.cpp:141:bool HeapGcState::TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& field, BaseObject*& fromObj,
runtime/src/Heap/z/zRelocate.cpp:151:                "HeapGcState::TryUpdateRefFieldImpl", provenance);
runtime/src/Heap/z/zRelocate.cpp:184:bool HeapGcState::TryUpdateRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const
runtime/src/Heap/z/zRelocate.cpp:194:void HeapGcState::RemapYoungRoots()
runtime/src/Heap/z/zRelocate.cpp:243:void HeapGcState::StartRelocationTasks(ZGenerationId generation)
runtime/src/Heap/z/zRelocate.cpp:252:bool HeapGcState::Preforward()
runtime/src/Heap/z/zRelocate.cpp:350:void EnsureRouteDomainMembership(HeapGcState* collector, BaseObject* obj)
runtime/src/Heap/z/zRelocate.cpp:432:bool ForceRootRouteDomainWhileForwardable(HeapGcState* collector, BaseObject* obj)
runtime/src/Heap/z/zRelocate.cpp:471:bool HeapGcState::CasInstallResolvedTarget(RefField<>& field, MAddress expected, zaddress target,
runtime/src/Heap/z/zRelocate.cpp:505:BaseObject* HeapGcState::ResolveMinorReference(RefField<>& field, const ScopedStopTheWorld* stw) const
runtime/src/Heap/z/zRelocate.cpp:528:BaseObject* HeapGcState::ResolveMinorReference(RootSlot& root, const ScopedStopTheWorld* stw) const
runtime/src/Heap/z/zRelocate.cpp:549:bool HeapGcState::FixMinorEvacuatedSlot(RefField<>& field, BaseObject* knownBase,
runtime/src/Heap/z/zRelocate.cpp:615:        EnsureRouteDomainMembership(const_cast<HeapGcState*>(this), target);
runtime/src/Heap/z/zRelocate.cpp:616:        current = const_cast<HeapGcState*>(this)->ForwardObject(target, Generation::Young);
runtime/src/Heap/z/zRelocate.cpp:627:            "HeapGcState::FixMinorEvacuatedSlot.unresolved", target,
runtime/src/Heap/z/zRelocate.cpp:659:bool HeapGcState::FixMinorEvacuatedSlot(RootSlot& root, const ScopedStopTheWorld* stw) const
runtime/src/Heap/z/zRelocate.cpp:678:        (void)ForceRootRouteDomainWhileForwardable(const_cast<HeapGcState*>(this), target);
runtime/src/Heap/z/zRelocate.cpp:679:        current = const_cast<HeapGcState*>(this)->ForwardObject(target, Generation::Young);
runtime/src/Heap/z/zRelocate.cpp:684:            if (ForceRootRouteDomainWhileForwardable(const_cast<HeapGcState*>(this), target)) {
runtime/src/Heap/z/zRelocate.cpp:685:                current = const_cast<HeapGcState*>(this)->ForwardObject(target, Generation::Young);
runtime/src/Heap/z/zRelocate.cpp:696:            "HeapGcState::FixMinorEvacuatedSlot", provenance);
runtime/src/Heap/z/zRelocate.cpp:703:            "HeapGcState::FixMinorEvacuatedSlot.unresolved", target,
runtime/src/Heap/z/zRelocate.cpp:715:bool HeapGcState::FixMinorEvacuatedSlot(DerivedSlot& derived, BaseObject* knownBase,
runtime/src/Heap/z/zRelocate.cpp:728:void HeapGcState::FixMinorRootSlots(const ScopedStopTheWorld* stw)
runtime/src/Heap/z/zRelocate.cpp:741:void HeapGcState::EvacuateYoungRegions(const std::vector<BaseObject*>& reachableVec,
runtime/src/Heap/z/zRelocate.cpp:956:static BaseObject* RemapPromotedField(HeapGcState& collector, RefField<>& field, zpointer observed)
runtime/src/Heap/z/zRelocate.cpp:984:    HeapGcState& collector = Heap::GetHeap().GetCollector();
runtime/src/Heap/z/zRelocate.cpp:1031:                        BaseObject* target = RemapPromotedField(Heap::GetHeap().GetCollector(), field, observed);
runtime/src/Heap/z/zRelocate.cpp:1235:    EnsureRouteDomainMembership(&Heap::GetHeap().GetCollector(), obj);
runtime/src/Heap/z/zRelocate.cpp:1418:BaseObject* HeapGcState::ForwardObject(BaseObject* obj, Generation generation)
runtime/src/Heap/z/zRelocate.cpp:1449:BaseObject* HeapGcState::ForwardObjectExclusive(BaseObject* obj)
runtime/src/Heap/z/zRelocate.cpp:1829:    HeapGcState& collector = reinterpret_cast<HeapGcState&>(Heap::GetHeap().GetCollector());
runtime/src/Heap/z/zMark.hpp:496:    // HeapGcState&, can spell it. Phase C changes that one body -- as in ZGC's
runtime/src/Heap/z/zGeneration.cpp:251:void HeapGcState::DoYoungGarbageCollection()
runtime/src/Heap/z/zGeneration.cpp:290:static HeapGcState& TheCollector()
runtime/src/Heap/z/zGeneration.cpp:292:    return static_cast<HeapGcState&>(Heap::GetHeap().GetCollector());
runtime/src/Heap/z/zGeneration.cpp:345:    if (HeapGcState::testMarkStartState) {
runtime/src/Heap/z/zGeneration.cpp:346:        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::Begin, mark.get());
runtime/src/Heap/z/zGeneration.cpp:352:    if (HeapGcState::testMarkStartState) {
runtime/src/Heap/z/zGeneration.cpp:353:        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::BeforeRetire, mark.get());
runtime/src/Heap/z/zGeneration.cpp:377:    if (HeapGcState::testMarkStartState) {
runtime/src/Heap/z/zGeneration.cpp:378:        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::BeforeSequence, mark.get());
runtime/src/Heap/z/zGeneration.cpp:388:    if (HeapGcState::testMarkStartState) {
runtime/src/Heap/z/zGeneration.cpp:389:        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::BeforeDomain, mark.get());
runtime/src/Heap/z/zGeneration.cpp:396:    if (HeapGcState::testMarkStartState) {
runtime/src/Heap/z/zGeneration.cpp:397:        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::BeforeRemembered, mark.get());
runtime/src/Heap/z/zGeneration.cpp:405:    if (HeapGcState::testMarkStartState) {
runtime/src/Heap/z/zGeneration.cpp:406:        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::Complete, mark.get());
runtime/src/Heap/z/zGeneration.cpp:410:    if (HeapGcState::testYoungMarkStarted) {
runtime/src/Heap/z/zGeneration.cpp:411:        HeapGcState::testYoungMarkStarted();
runtime/src/Heap/z/zGeneration.cpp:617:        if (HeapGcState::testYoungMarkCompleted) {
runtime/src/Heap/z/zGeneration.cpp:618:            HeapGcState::testYoungMarkCompleted();
runtime/src/Heap/z/zGeneration.cpp:685:        // same place HeapGcState::PostTrace drains them for a major (RelocationSet.cpp:73-78).
runtime/src/Heap/z/zGeneration.cpp:729:    HeapGcState& collector = TheCollector();
runtime/src/Heap/z/zGeneration.cpp:827:    Heap::GetHeap().GetCollector().ProcessFinalizers();
runtime/src/Heap/z/zGeneration.cpp:1055:void HeapGcState::DoGarbageCollection(ZGenerationId generation)
runtime/src/Heap/z/zGeneration.cpp:1070:    if (HeapGcState::testMarkStartState) {
runtime/src/Heap/z/zGeneration.cpp:1071:        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::Begin, mark.get());
runtime/src/Heap/z/zGeneration.cpp:1077:    if (HeapGcState::testMarkStartState) {
runtime/src/Heap/z/zGeneration.cpp:1078:        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::BeforeRetire, mark.get());
runtime/src/Heap/z/zGeneration.cpp:1088:    if (HeapGcState::testMarkStartState) {
runtime/src/Heap/z/zGeneration.cpp:1089:        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::BeforeSequence, mark.get());
runtime/src/Heap/z/zGeneration.cpp:1100:    if (HeapGcState::testMarkStartState) {
runtime/src/Heap/z/zGeneration.cpp:1101:        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::BeforeDomain, mark.get());
runtime/src/Heap/z/zGeneration.cpp:1108:    if (HeapGcState::testMarkStartState) {
runtime/src/Heap/z/zGeneration.cpp:1109:        HeapGcState::testMarkStartState(_cycle, MarkStartPoint::Complete, mark.get());
runtime/src/Heap/z/zGeneration.cpp:1145:    HeapGcState& collector = TheCollector();
runtime/src/Heap/z/zGeneration.cpp:1216:        if (HeapGcState::testOldMarkStarted) HeapGcState::testOldMarkStarted();
runtime/src/Heap/z/zGeneration.cpp:1270:    HeapGcState& collector = TheCollector();
runtime/src/Heap/z/zGeneration.cpp:1282:void HeapGcState::PreGarbageCollection(ZGenerationId generation, bool isConcurrent, uint64_t gcIndex)
runtime/src/Heap/z/zGeneration.cpp:1467:void HeapGcState::PostGarbageCollection(ZGenerationId generation, uint64_t gcIndex)
runtime/src/Heap/z/zGeneration.cpp:1480:void HeapGcState::ForwardFromSpace(ZGenerationId generation)
runtime/src/Heap/z/zGeneration.cpp:1496:void HeapGcState::RefineFromSpace()
runtime/src/Heap/z/zBarrier.cpp:915:    const bool inFrom = Heap::GetHeap().GetCollector().IsFromObject(target);
runtime/src/Heap/z/zStat.cpp:659:void HeapGcState::UpdateGCStats()
runtime/src/Heap/z/zCrossVM.cpp:370:                BaseObject* target = Heap::GetHeap().GetCollector().GetAndTryTagObj(HeapGcState::RefSlotKind::STRONG, object, field);
runtime/src/Heap/z/zHeapIterator.cpp:163:    auto& collector = static_cast<HeapGcState&>(Heap::GetHeap().GetCollector());
runtime/src/Heap/z/zHeapIterator.cpp:179:    auto& collector = static_cast<HeapGcState&>(Heap::GetHeap().GetCollector());
runtime/src/Heap/z/zHeap.cpp:109:    collectorImpl.reset(new HeapGcState());
runtime/src/Heap/z/zHeap.cpp:167:HeapGcState& Heap::GetCollector() { return *collectorImpl; }
runtime/src/Heap/z/zHeap.cpp:168:const HeapGcState& Heap::GetCollector() const { return *collectorImpl; }
runtime/src/Heap/z/zHeap.cpp:250:bool Heap::IsGhostFromObject(BaseObject* obj) const { return GetCollector().IsGhostFromObject(obj); }
runtime/src/Heap/z/zHeap.cpp:252:bool Heap::IsUnmovableFromObject(BaseObject* obj) const { return GetCollector().IsUnmovableFromObject(obj); }
runtime/src/Heap/z/zHeap.cpp:256:    return GetCollector().ForwardObject(fromVersion, generation);
runtime/src/Heap/z/zDriver.cpp:262:    Heap::GetHeap().GetCollector().PreGarbageCollection(generation, reason != GC_REASON_YOUNG, gcIndex);
runtime/src/Heap/z/zDriver.cpp:293:    Heap::GetHeap().GetCollector().PostGarbageCollection(generation, gcIndex);
runtime/src/Heap/z/zDriver.cpp:310:        Heap::GetHeap().GetCollector().UpdateGCStats();
runtime/src/Heap/z/zRelocate.hpp:155:    friend class HeapGcState;
```
