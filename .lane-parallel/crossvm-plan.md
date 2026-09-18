# Cross-VM state extraction plan (read-only)
待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`（若采纳为当前文档）。

Coordinates: HEAD `f49bd78712f5cd31043a1456f85292050f3bc8ce` plus this lane's active working-tree edits; line numbers below are current read coordinates. Product untouched by this planning subtask.

## Concrete ownership and headers

Add infrastructure class `ZCrossVM` in existing allowed `zCrossVM.*`, **one value member of Heap**, no inheritance from HeapGcState and no renamed collector facade. Heap accessor returns that unique member; every state consumer must converge on it. Keep this explicitly in infrastructure-difference table: foreign ownership graphs, exported stable IDs and managed cross-runtime cycle-resolution callbacks have no ZGC direct counterpart. Comparison anchors: ZGC zMark.cpp:412-415 liveness deduplication is not foreign-owner deduplication; zReferenceProcessor.cpp:175-203 weak discovery must not strengthen edges; zUncoloredRoot.inline.hpp:62-69 current uncolored-root identity; zGeneration.cpp:1261 mark-end retains forwarding authority (existing source comments anchor these).

Generation enum is in zGenerationId.hpp:10 (uint8_t); use that direct header.

Move `CrossRefHandler`, `ValueRoot`, `ValueRootHash`, `ValueRootSet/List/Map` with the new class. **Move ValueRoot constructor and Stage bodies out of header**: constructor calls Heap::IsHeapAddress/Heap::page, so retaining inline definition would create `zHeap.hpp -> zCrossVM.hpp -> zHeap.hpp` incomplete-type cycle. Forward-declare BaseObject; use direct type/header dependencies for Generation, ForwardingStage, WorkStack rather than zMark.hpp. All method bodies accessing Heap/ZGeneration/collector authority live in zCrossVM.cpp. Inspect `zForwardingLookup.hpp` transitive includes before selecting it as enum provider; if it drags Heap, forward-declare the enum with exact underlying type and move default-enumerator arguments to overload bodies. Do not include zMark.hpp from zCrossVM.hpp.

## Exact state migration

From zMark.hpp:478-496: `externMtx`, `discoveredExternObjects`, `cycleResolverMtx`, `cycleWorkStackMtx`, `cycleRefWorkStack`, `cycleRefProgress`, `resurrectExportMtx`, `resurrectedExportObjectes`, `resurrectedExportObjectesForwardPhase`.
Move `cycleRefHandlerForTest` (MRT_GC_UNIT_TESTS), `ExportOwnershipTestObservation` and static `testExportOwnershipResult` (MRT_TESTABLE_INTERNALS) with their same gates. **Do not migrate markedObjectCount**: it is generation marking state, not cross-VM ownership.

## Method migration list

| Methods (exact spellings) | Current bodies | Dependency/action |
|---|---|---|
| GetCrossRefHandler, ResolveCycleRef, PostResolveCycleTask | zCrossVM.cpp:89,99,217 | Mechanical owner change; Heap::ResolveCycleRef at zHeap.cpp:172 calls unique new member |
| ResurrectExportObject, VisitAllResurrectExportObjects, PrepareCycleRef, MergeResurrectExportObjects, SetCycleRefHandlerForTest | zMark.hpp:409,428,595,606,569 | Move bodies out of header; incoming export producer zHeap.cpp:458 changes to new member |
| ResolveCurrentValueRoot, CurrentizeValueRootSet, CurrentizeValueRootMap | zGeneration.cpp:890,924,935 | Call existing real remap/validation authority explicitly until that authority migrates; never clone its logic |
| VisitSurrectedExportRoots | zGeneration.cpp:956 | Mechanically migrate full owner-lock/rekey/visit body |
| VisitMinorValueRoots | zMark.cpp:235 | Move body; preserve gMinorRootOrigin telemetry via existing external TLS declaration, not a second TLS instance |
| FindUselessExternObjects | zMark.cpp:934 | Mechanical move; caller zGeneration.cpp:827 |
| PreforwardDiscoveredExternObjects, PreforwardAllResurrectExportFromObjects | zRelocate.cpp:287,294 | Move bodies; old/young relocation call sites below |
| ObserveExportOwnershipForTest | zExportOwnershipTestObservations.hpp:8 | Move class qualifier; hook definition currently zTracing.cpp:33; retain callback outside both locks |
| ProcessExportRoots | zMark.cpp:883 | **Not purely owner replacement**: depends on MarkOldObjectIfActive, RunMajorStripeMark, markedObjectCount, GetAndTryTagObj. Move ownership traversal to new class, explicitly route marking/closure through true old-generation domain. Do not copy marking state into ZCrossVM or pass synthetic result values. Transitional explicit Heap::GetHeap().GetCollector() calls permissible only as WIP, not final HeapGcState deletion. |

`VisitAllResurrectExportObjects` has only its definition in product/tests search: deletion rather than migration is valid after baseline/candidate paired evidence.

`EnumAllExportRoots` (zRootsIterator.cpp:194) currently reads Heap export-handle roots and uses EnumRefFieldRoot; it need not move with cross-VM ownership containers. Its producer output reaches ZGenerationOld::mark_end / ProcessExportRoots at zGeneration.cpp:1222; preserve this phase order. `ShouldIgnoreRequest` happens to reside zCrossVM.cpp but is unrelated; do not rename it onto ZCrossVM.

## Producer -> consumer -> phase order

1. Heap export resurrection insertion zHeap.cpp:458 -> ResurrectExportObject currentizes IncomingNew identity under resurrectExportMtx -> young roots zMark.cpp:437 / old roots zMark.cpp:374 -> rekey each carrier **before** visitor/mark consumer.
2. Heap::VisitAllExportRoots -> EnumAllExportRoots zRootsIterator.cpp:194 -> generation oldMarkForeignRoots -> ProcessExportRoots zGeneration.cpp:1222: dedup owner under externMtx, mark closure, strong-edge-only ownership walk -> FindUselessExternObjects zGeneration.cpp:827 (rekey discovered map).
3. PostTrace zRelocationSet.cpp:65-69: observe(false) -> PrepareCycleRef (currentize both maps, splice, clear discovered) -> observe(true). Preserve handoff before relocation receipt authority is reset.
4. Major preforward zRelocate.cpp:351-352 and minor branches :924-925 / :951-952 -> PreforwardDiscoveredExternObjects + PreforwardAllResurrectExportFromObjects, while relevant forwarding tables are installed.
5. Young completion zGeneration.cpp:772 -> MergeResurrectExportObjects(Young); old completion :1368 -> MergeResurrectExportObjects(Old) -> :1369 PostResolveCycleTask.
6. Scheduler CjScheduler.cpp:855 -> Heap::ResolveCycleRef -> unique ZCrossVM::ResolveCycleRef -> managed adapter; callback may safepoint -> GC can currentize maps during callback -> resolver reacquires/re-finds by stable export ID -> stores per-ID progress -> completion deactivates export record.

## Locking/order that must survive verbatim

- ScopedObjectAccess precedes both resolver and carrier acquisition. Resolver uses try-lock cycleResolverMtx then try-lock cycleWorkStackMtx; failed acquisition reposts, not blocking under saferegion.
- cycleResolverMtx serializes resolvers only; no GC consumer acquires it. Release cycleWorkStackMtx before every managed callback and reacquire after return. Keep carrier entry present throughout callback. Re-find iterator/address by stable export ID for every invocation; do not cache map iterators across callback.
- Increment/store cycleRefProgress **before** observing subsequent phase change; preserve 100-owner budget and enteredInRelocate distinction.
- Sets/maps are rebuilt and swapped under their current owner lock; keys as well as values currentized, Stage/color/generation metadata retained. IncomingNew must use validation identity path; stored roots use forwarding table generation, not visiting generation.
- Root visitors release resurrectExportMtx before taking cycleWorkStackMtx. Do not turn these sequential scopes into nested locks. FindUselessExternObjects uses externMtx; producer ProcessExportRoots releases externMtx before marking/closure traversal.
- PrepareCycleRef currently uses only cycleWorkStackMtx and relies on phase quiescence of discovered producer. Preserve phase entry; adding externMtx or moving earlier requires separate concurrency proof, not mechanical cleanup.
- Existing ResolveCycleRef reads/clears resurrection sets outside resurrectExportMtx in parts of its flow; this plan records existing behavior, does not justify broad concurrency changes or silently add locking.

## Tests and fixture migration

Direct private-container access: `RelocationReceiptTestAccess` in clear_entries_product_unit.cpp, test_young_weak.cpp, test_cycle_ref_saferegion.cpp; `ZGenerationRootTestAccess` in test_generation_cycle_context.cpp and ohos_host/ohos_cycle_unit.cpp. Add only these same gated friends to ZCrossVM; change each container access to the Heap-owned ZCrossVM instance, while other marking/remapping helper access remains on its true owner. No duplicate object in fixtures. Calls to VisitMinorValueRoots/VisitSurrectedExportRoots, preforward methods, SetCycleRefHandlerForTest, ResolveCycleRef/PostResolveCycleTask follow unique member. Hook consumers test_young_weak.cpp:850,880 change static class qualifier. `run_standalone.sh:55,104,112,120` binds OHOS nm/disassembly checks to exact PostResolveCycleTask symbol; must update to ZCrossVM and verify actual executable symbol/disassembly, not delete checks.

## Mechanical inventory (complete matched lines, including declarations/comments)

Command: `rg -n '<pattern stored in generating script / inventory below>' runtime/src runtime/tests/gc_unit`; rc=0. Matches below are source occurrences, not all semantic call sites: declarations, method definitions, and comments are deliberately retained so implementer can distinguish each. Historical docs/evidence were not searched as product consumers.

```text
runtime/tests/gc_unit/run_standalone.sh:55:      'MapleRuntime::HeapGcState::PostResolveCycleTask()'; do
runtime/tests/gc_unit/run_standalone.sh:104:      'MapleRuntime::HeapGcState::PostResolveCycleTask()'; do
runtime/tests/gc_unit/run_standalone.sh:112:      'MapleRuntime::HeapGcState::PostResolveCycleTask()'; do
runtime/tests/gc_unit/run_standalone.sh:120:    '/<MapleRuntime::HeapGcState::PostResolveCycleTask()>/,/^$/p' >"$post_disassembly"
runtime/tests/gc_unit/clear_entries_product_unit.cpp:199:    static void SeedValueRoots(HeapGcState& collector, BaseObject* value)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:202:            std::lock_guard<std::mutex> lock(collector.resurrectExportMtx);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:203:            collector.resurrectedExportObjectes.clear();
runtime/tests/gc_unit/clear_entries_product_unit.cpp:204:            collector.resurrectedExportObjectesForwardPhase.clear();
runtime/tests/gc_unit/clear_entries_product_unit.cpp:205:            collector.resurrectedExportObjectes.insert(value);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:206:            collector.resurrectedExportObjectesForwardPhase.insert(value);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:208:        std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:209:        collector.cycleRefWorkStack.clear();
runtime/tests/gc_unit/clear_entries_product_unit.cpp:210:        collector.cycleRefWorkStack[value].push_back(value);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:213:    static bool AllValueRootCarriersEqual(HeapGcState& collector, BaseObject* value)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:217:            std::lock_guard<std::mutex> lock(collector.resurrectExportMtx);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:218:            resurrected = collector.resurrectedExportObjectes.size() == 1 &&
runtime/tests/gc_unit/clear_entries_product_unit.cpp:219:                collector.resurrectedExportObjectes.count(value) == 1 &&
runtime/tests/gc_unit/clear_entries_product_unit.cpp:220:                collector.resurrectedExportObjectesForwardPhase.size() == 1 &&
runtime/tests/gc_unit/clear_entries_product_unit.cpp:221:                collector.resurrectedExportObjectesForwardPhase.count(value) == 1;
runtime/tests/gc_unit/clear_entries_product_unit.cpp:223:        std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:224:        auto it = collector.cycleRefWorkStack.find(value);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:225:        return resurrected && collector.cycleRefWorkStack.size() == 1 &&
runtime/tests/gc_unit/clear_entries_product_unit.cpp:226:            it != collector.cycleRefWorkStack.end() && it->second.size() == 1 &&
runtime/tests/gc_unit/clear_entries_product_unit.cpp:232:        std::lock_guard<std::mutex> lock(collector.resurrectExportMtx);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:233:        return collector.resurrectedExportObjectes.size() == 1 &&
runtime/tests/gc_unit/clear_entries_product_unit.cpp:234:            collector.resurrectedExportObjectes.count(value) == 1 &&
runtime/tests/gc_unit/clear_entries_product_unit.cpp:235:            collector.resurrectedExportObjectesForwardPhase.size() == 1 &&
runtime/tests/gc_unit/clear_entries_product_unit.cpp:236:            collector.resurrectedExportObjectesForwardPhase.count(value) == 1;
runtime/tests/gc_unit/clear_entries_product_unit.cpp:239:    static std::vector<BaseObject*> VisitMinorValueRoots(HeapGcState& collector)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:242:        collector.VisitMinorValueRoots([&visited](BaseObject* value) { visited.push_back(value); });
runtime/tests/gc_unit/clear_entries_product_unit.cpp:248:    static std::vector<BaseObject*> EnumMajorValueRoots(HeapGcState& collector)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:251:        collector.VisitSurrectedExportRoots([&](BaseObject* object) { visited.push_back(object); });
runtime/tests/gc_unit/clear_entries_product_unit.cpp:256:    static void RunLateValueRootRekey(HeapGcState& collector)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:258:        collector.PreforwardDiscoveredExternObjects(Generation::Old);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:259:        collector.PreforwardAllResurrectExportFromObjects(Generation::Old);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:680:LateBackfillState PrepareValueRootForwarding(GcHeapFixture& fx, HeapGcState& collector)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:706:void CompleteValueRootCoverage()
runtime/tests/gc_unit/clear_entries_product_unit.cpp:716:GC_OTHER_VM_TEST(ValueRootCurrentization, MinorConsumerRewritesEveryCarrierBeforeCoverage)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:720:    LateBackfillState state = PrepareValueRootForwarding(fx, collector);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:721:    RelocationReceiptTestAccess::SeedValueRoots(collector, state.from);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:724:        RelocationReceiptTestAccess::VisitMinorValueRoots(collector);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:727:        RelocationReceiptTestAccess::AllValueRootCarriersEqual(collector, state.to);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:730:    CompleteValueRootCoverage();
runtime/tests/gc_unit/clear_entries_product_unit.cpp:734:        RelocationReceiptTestAccess::VisitMinorValueRoots(collector);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:749:GC_OTHER_VM_TEST(ValueRootCurrentization, MajorConsumerRewritesEveryCarrierBeforeCoverage)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:753:    LateBackfillState state = PrepareValueRootForwarding(fx, collector);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:754:    RelocationReceiptTestAccess::SeedValueRoots(collector, state.from);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:757:        RelocationReceiptTestAccess::EnumMajorValueRoots(collector);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:760:        RelocationReceiptTestAccess::AllValueRootCarriersEqual(collector, state.to);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:763:    CompleteValueRootCoverage();
runtime/tests/gc_unit/clear_entries_product_unit.cpp:767:        RelocationReceiptTestAccess::EnumMajorValueRoots(collector);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:782:GC_OTHER_VM_TEST(ValueRootCurrentization, InsertionAndLateRekeyShareCurrentAuthority)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:786:    LateBackfillState state = PrepareValueRootForwarding(fx, collector);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:791:    collector.ResurrectExportObject(state.to);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:793:    collector.ResurrectExportObject(state.to);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:797:    RelocationReceiptTestAccess::SeedValueRoots(collector, state.from);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:798:    RelocationReceiptTestAccess::RunLateValueRootRekey(collector);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:800:        RelocationReceiptTestAccess::AllValueRootCarriersEqual(collector, state.to);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:1312:        state = PrepareValueRootForwarding(fx, collector);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2625:        collector.ResurrectExportObject(second);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2627:        collector.ResurrectExportObject(second);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2650:            collector.ResurrectExportObject(current);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2652:            collector.ResurrectExportObject(current);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2662:    const auto visited = major ? RelocationReceiptTestAccess::EnumMajorValueRoots(collector)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2663:                               : RelocationReceiptTestAccess::VisitMinorValueRoots(collector);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2672:GC_OTHER_VM_TEST(ValueRootCurrentization, IncomingCurrentCompactDestinationKeepsIdentity)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2677:GC_OTHER_VM_TEST(ValueRootCurrentization, NonOverlappingCurrentDestinationKeepsIdentity)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2682:GC_OTHER_VM_TEST(ValueRootCurrentization, ExternalCurrentDestinationKeepsIdentity)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2687:GC_OTHER_VM_TEST(ValueRootCurrentization, MajorIncomingCurrentCompactDestinationKeepsIdentity)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2692:GC_OTHER_VM_TEST(ValueRootCurrentization, MajorExternalCurrentDestinationKeepsIdentity)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2747:GC_OTHER_VM_TEST(ValueRootCurrentization, MinorCurrentOldRootSurvivesYoungColorFlip)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2752:GC_OTHER_VM_TEST(ValueRootCurrentization, MajorCurrentOldRootSurvivesYoungColorFlip)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2757:GC_OTHER_VM_TEST(ValueRootCurrentization, MinorStoredCurrentRootRemapsAfterOldColorFlip)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2762:GC_OTHER_VM_TEST(ValueRootCurrentization, MajorStoredCurrentRootRemapsAfterOldColorFlip)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2834:GC_OTHER_VM_TEST(ValueRootCurrentization, ExportEntryCurrentCompactDestinationKeepsIdentity)
runtime/tests/gc_unit/test_generation_cycle_context.cpp:57:            std::lock_guard<std::mutex> lock(collector.resurrectExportMtx);
runtime/tests/gc_unit/test_generation_cycle_context.cpp:58:            collector.resurrectedExportObjectes.insert(objects[2]);
runtime/tests/gc_unit/test_generation_cycle_context.cpp:59:            collector.resurrectedExportObjectesForwardPhase.insert(objects[3]);
runtime/tests/gc_unit/test_generation_cycle_context.cpp:62:            std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
runtime/tests/gc_unit/test_generation_cycle_context.cpp:63:            collector.cycleRefWorkStack[objects[4]].push_back(objects[5]);
runtime/tests/gc_unit/test_generation_cycle_context.cpp:75:            std::lock_guard<std::mutex> lock(collector.resurrectExportMtx);
runtime/tests/gc_unit/test_generation_cycle_context.cpp:76:            collector.resurrectedExportObjectes.erase(objects[2]);
runtime/tests/gc_unit/test_generation_cycle_context.cpp:77:            collector.resurrectedExportObjectesForwardPhase.erase(objects[3]);
runtime/tests/gc_unit/test_generation_cycle_context.cpp:80:            std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
runtime/tests/gc_unit/test_generation_cycle_context.cpp:81:            collector.cycleRefWorkStack.erase(objects[4]);
runtime/tests/gc_unit/test_young_weak.cpp:132:    static void SeedValueRoots(HeapGcState& collector, BaseObject* value)
runtime/tests/gc_unit/test_young_weak.cpp:135:            std::lock_guard<std::mutex> lock(collector.resurrectExportMtx);
runtime/tests/gc_unit/test_young_weak.cpp:136:            collector.resurrectedExportObjectes.clear();
runtime/tests/gc_unit/test_young_weak.cpp:137:            collector.resurrectedExportObjectesForwardPhase.clear();
runtime/tests/gc_unit/test_young_weak.cpp:138:            collector.resurrectedExportObjectes.insert(value);
runtime/tests/gc_unit/test_young_weak.cpp:139:            collector.resurrectedExportObjectesForwardPhase.insert(value);
runtime/tests/gc_unit/test_young_weak.cpp:141:        std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
runtime/tests/gc_unit/test_young_weak.cpp:142:        collector.cycleRefWorkStack.clear();
runtime/tests/gc_unit/test_young_weak.cpp:143:        collector.discoveredExternObjects.clear();
runtime/tests/gc_unit/test_young_weak.cpp:144:        collector.cycleRefWorkStack[value].push_back(value);
runtime/tests/gc_unit/test_young_weak.cpp:147:    static bool AllValueRootsEqual(HeapGcState& collector, BaseObject* value)
runtime/tests/gc_unit/test_young_weak.cpp:150:            std::lock_guard<std::mutex> lock(collector.resurrectExportMtx);
runtime/tests/gc_unit/test_young_weak.cpp:151:            if (collector.resurrectedExportObjectes.size() != 1 ||
runtime/tests/gc_unit/test_young_weak.cpp:152:                collector.resurrectedExportObjectes.count(value) != 1 ||
runtime/tests/gc_unit/test_young_weak.cpp:153:                collector.resurrectedExportObjectesForwardPhase.size() != 1 ||
runtime/tests/gc_unit/test_young_weak.cpp:154:                collector.resurrectedExportObjectesForwardPhase.count(value) != 1) {
runtime/tests/gc_unit/test_young_weak.cpp:158:        std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
runtime/tests/gc_unit/test_young_weak.cpp:159:        auto it = collector.cycleRefWorkStack.find(value);
runtime/tests/gc_unit/test_young_weak.cpp:160:        return collector.cycleRefWorkStack.size() == 1 && it != collector.cycleRefWorkStack.end() &&
runtime/tests/gc_unit/test_young_weak.cpp:166:        std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
runtime/tests/gc_unit/test_young_weak.cpp:167:        auto it = collector.cycleRefWorkStack.find(key);
runtime/tests/gc_unit/test_young_weak.cpp:168:        return collector.discoveredExternObjects.empty() && collector.cycleRefWorkStack.size() == owners &&
runtime/tests/gc_unit/test_young_weak.cpp:169:            it != collector.cycleRefWorkStack.end() && it->second.size() == 1 && it->second.front() == value;
runtime/tests/gc_unit/test_young_weak.cpp:174:        std::lock_guard<std::mutex> lock(collector.externMtx);
runtime/tests/gc_unit/test_young_weak.cpp:175:        auto it = collector.discoveredExternObjects.find(key);
runtime/tests/gc_unit/test_young_weak.cpp:176:        return collector.discoveredExternObjects.size() == owners &&
runtime/tests/gc_unit/test_young_weak.cpp:177:            it != collector.discoveredExternObjects.end() && it->second.size() == 1 &&
runtime/tests/gc_unit/test_young_weak.cpp:181:    static bool MinorFinishedValueRootsEqual(HeapGcState& collector, BaseObject* value)
runtime/tests/gc_unit/test_young_weak.cpp:184:            std::lock_guard<std::mutex> lock(collector.resurrectExportMtx);
runtime/tests/gc_unit/test_young_weak.cpp:185:            if (collector.resurrectedExportObjectes.size() != 1 ||
runtime/tests/gc_unit/test_young_weak.cpp:186:                collector.resurrectedExportObjectes.count(value) != 1 ||
runtime/tests/gc_unit/test_young_weak.cpp:187:                !collector.resurrectedExportObjectesForwardPhase.empty()) {
runtime/tests/gc_unit/test_young_weak.cpp:191:        std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
runtime/tests/gc_unit/test_young_weak.cpp:192:        auto it = collector.cycleRefWorkStack.find(value);
runtime/tests/gc_unit/test_young_weak.cpp:193:        return collector.cycleRefWorkStack.size() == 1 && it != collector.cycleRefWorkStack.end() &&
runtime/tests/gc_unit/test_young_weak.cpp:235:class ValueRootMarkObservation {
runtime/tests/gc_unit/test_young_weak.cpp:237:    explicit ValueRootMarkObservation(std::function<void()> observe) : observe(std::move(observe))
runtime/tests/gc_unit/test_young_weak.cpp:245:    ~ValueRootMarkObservation()
runtime/tests/gc_unit/test_young_weak.cpp:253:    inline static ValueRootMarkObservation* current = nullptr;
runtime/tests/gc_unit/test_young_weak.cpp:256:struct ValueRootRoute {
runtime/tests/gc_unit/test_young_weak.cpp:263:ValueRootRoute PrepareValueRootRoute(GcHeapFixture& fx, bool destinationYoung)
runtime/tests/gc_unit/test_young_weak.cpp:265:    ValueRootRoute route;
runtime/tests/gc_unit/test_young_weak.cpp:298:bool IsValueRootMarked(const ValueRootRoute& route)
runtime/tests/gc_unit/test_young_weak.cpp:755:GC_OTHER_VM_TEST(ValueRootCurrentization, MinorRuntimeDispatchMarksCurrentAndWritesBack)
runtime/tests/gc_unit/test_young_weak.cpp:761:    ValueRootRoute route = PrepareValueRootRoute(fx, true);
runtime/tests/gc_unit/test_young_weak.cpp:775:    RelocationReceiptTestAccess::SeedValueRoots(collector, route.from);
runtime/tests/gc_unit/test_young_weak.cpp:784:    ValueRootMarkObservation closure([&] { currentMarked |= IsValueRootMarked(route); });
runtime/tests/gc_unit/test_young_weak.cpp:788:        RelocationReceiptTestAccess::MinorFinishedValueRootsEqual(collector, route.to);
runtime/tests/gc_unit/test_young_weak.cpp:793:        RelocationReceiptTestAccess::MinorFinishedValueRootsEqual(collector, route.to);
runtime/tests/gc_unit/test_young_weak.cpp:850:        HeapGcState::testExportOwnershipResult = [&](const ExportOwnershipTestObservation& observed) {
runtime/tests/gc_unit/test_young_weak.cpp:880:        HeapGcState::testExportOwnershipResult = nullptr;
runtime/tests/gc_unit/test_young_weak.cpp:924:GC_OTHER_VM_TEST(ValueRootCurrentization, MajorProducerConsumerCurrentizesBeforeMark)
runtime/tests/gc_unit/test_young_weak.cpp:929:GC_OTHER_VM_TEST(ValueRootCurrentization, MajorExportOwnersSharePremarkedCycle)
runtime/tests/gc_unit/test_young_weak.cpp:934:GC_OTHER_VM_TEST(ValueRootCurrentization, MajorDriverPairsExportOwnersBeforeHandoff)
runtime/tests/gc_unit/ohos_host/ohos_cycle_unit.cpp:27:        std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
runtime/tests/gc_unit/ohos_host/ohos_cycle_unit.cpp:28:        collector.cycleRefWorkStack.emplace(ValueRoot(object),
runtime/tests/gc_unit/ohos_host/ohos_cycle_unit.cpp:29:                                            ValueRootList{});
runtime/tests/gc_unit/ohos_host/ohos_cycle_unit.cpp:34:        std::lock_guard<std::mutex> lock(collector.cycleWorkStackMtx);
runtime/tests/gc_unit/ohos_host/ohos_cycle_unit.cpp:35:        collector.cycleRefWorkStack.clear();
runtime/tests/gc_unit/ohos_host/ohos_cycle_unit.cpp:37:    static void PostResolveCycleTask(HeapGcState& collector) { collector.PostResolveCycleTask(); }
runtime/tests/gc_unit/ohos_host/ohos_cycle_unit.cpp:110:    ZGenerationRootTestAccess::PostResolveCycleTask(collector);
runtime/tests/gc_unit/ohos_host/ohos_cycle_unit.cpp:121:    ZGenerationRootTestAccess::PostResolveCycleTask(collector);
runtime/tests/gc_unit/test_cycle_ref_saferegion.cpp:50:        collector.cycleRefWorkStack[owner].push_back(target);
runtime/tests/gc_unit/test_cycle_ref_saferegion.cpp:52:    static void ClearCycleRoots(HeapGcState& collector) { collector.cycleRefWorkStack.clear(); }
runtime/tests/gc_unit/test_cycle_ref_saferegion.cpp:55:        collector.VisitSurrectedExportRoots(visitor);
runtime/tests/gc_unit/test_cycle_ref_saferegion.cpp:59:        collector.VisitMinorValueRoots(visitor);
runtime/tests/gc_unit/test_cycle_ref_saferegion.cpp:150:        collector.ResolveCycleRef();
runtime/tests/gc_unit/test_cycle_ref_saferegion.cpp:266:        collector.ResolveCycleRef();
runtime/tests/gc_unit/test_cycle_ref_saferegion.cpp:362:    collector.ResolveCycleRef();
runtime/tests/gc_unit/test_cycle_ref_saferegion.cpp:367:    collector.ResolveCycleRef();
runtime/tests/gc_unit/test_cycle_ref_saferegion.cpp:369:    collector.ResolveCycleRef();
runtime/src/CjScheduler.cpp:855:    Heap::GetHeap().ResolveCycleRef();
runtime/src/Heap/z/zHeap.hpp:86:    void ResolveCycleRef();
runtime/src/Heap/z/zRelocate.cpp:287:void HeapGcState::PreforwardDiscoveredExternObjects(Generation generation)
runtime/src/Heap/z/zRelocate.cpp:289:    std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
runtime/src/Heap/z/zRelocate.cpp:290:    CHECK(discoveredExternObjects.empty());
runtime/src/Heap/z/zRelocate.cpp:291:    CurrentizeValueRootMap(cycleRefWorkStack, generation);
runtime/src/Heap/z/zRelocate.cpp:294:void HeapGcState::PreforwardAllResurrectExportFromObjects(Generation generation)
runtime/src/Heap/z/zRelocate.cpp:296:    std::lock_guard<std::mutex> lg(resurrectExportMtx);
runtime/src/Heap/z/zRelocate.cpp:297:    CurrentizeValueRootSet(resurrectedExportObjectes, generation);
runtime/src/Heap/z/zRelocate.cpp:298:    CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase, generation);
runtime/src/Heap/z/zRelocate.cpp:351:        [&] { PreforwardDiscoveredExternObjects(Generation::Old); },
runtime/src/Heap/z/zRelocate.cpp:352:        [&] { PreforwardAllResurrectExportFromObjects(Generation::Old); }
runtime/src/Heap/z/zRelocate.cpp:924:            PreforwardDiscoveredExternObjects(Generation::Young);
runtime/src/Heap/z/zRelocate.cpp:925:            PreforwardAllResurrectExportFromObjects(Generation::Young);
runtime/src/Heap/z/zRelocate.cpp:951:            PreforwardDiscoveredExternObjects(Generation::Young);
runtime/src/Heap/z/zRelocate.cpp:952:            PreforwardAllResurrectExportFromObjects(Generation::Young);
runtime/src/Heap/z/zGeneration.cpp:772:        collector.MergeResurrectExportObjects(Generation::Young);
runtime/src/Heap/z/zGeneration.cpp:827:        Heap::GetHeap().GetCollector().FindUselessExternObjects();
runtime/src/Heap/z/zGeneration.cpp:890:BaseObject* HeapGcState::ResolveCurrentValueRoot(BaseObject* value, const void* owner, Generation generation,
runtime/src/Heap/z/zGeneration.cpp:924:void HeapGcState::CurrentizeValueRootSet(ValueRootSet& roots, Generation generation) const
runtime/src/Heap/z/zGeneration.cpp:926:    ValueRootSet current;
runtime/src/Heap/z/zGeneration.cpp:928:    for (const ValueRoot& value : roots) {
runtime/src/Heap/z/zGeneration.cpp:929:        current.insert(ValueRoot(ResolveCurrentValueRoot(value, &roots, generation, value.Stage()),
runtime/src/Heap/z/zGeneration.cpp:935:void HeapGcState::CurrentizeValueRootMap(
runtime/src/Heap/z/zGeneration.cpp:936:    ValueRootMap& roots, Generation generation) const
runtime/src/Heap/z/zGeneration.cpp:938:    ValueRootMap current;
runtime/src/Heap/z/zGeneration.cpp:941:        ValueRoot key(ResolveCurrentValueRoot(entry.first, &roots, generation, entry.first.Stage()),
runtime/src/Heap/z/zGeneration.cpp:943:        ValueRootList& values = current[key];
runtime/src/Heap/z/zGeneration.cpp:944:        for (const ValueRoot& value : entry.second) {
runtime/src/Heap/z/zGeneration.cpp:945:            values.emplace_back(ResolveCurrentValueRoot(value, &roots, generation, value.Stage()),
runtime/src/Heap/z/zGeneration.cpp:956:void HeapGcState::VisitSurrectedExportRoots(const std::function<void(BaseObject*)>& visitor)
runtime/src/Heap/z/zGeneration.cpp:959:        std::lock_guard<std::mutex> lg(resurrectExportMtx);
runtime/src/Heap/z/zGeneration.cpp:960:        CurrentizeValueRootSet(resurrectedExportObjectes, Generation::Old);
runtime/src/Heap/z/zGeneration.cpp:961:        CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase, Generation::Old);
runtime/src/Heap/z/zGeneration.cpp:962:        for (BaseObject* obj : resurrectedExportObjectes) {
runtime/src/Heap/z/zGeneration.cpp:965:        for (BaseObject* obj : resurrectedExportObjectesForwardPhase) {
runtime/src/Heap/z/zGeneration.cpp:969:    std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
runtime/src/Heap/z/zGeneration.cpp:970:    CurrentizeValueRootMap(cycleRefWorkStack, Generation::Old);
runtime/src/Heap/z/zGeneration.cpp:971:    auto it = cycleRefWorkStack.begin();
runtime/src/Heap/z/zGeneration.cpp:972:    while (it != cycleRefWorkStack.end()) {
runtime/src/Heap/z/zGeneration.cpp:1222:    TheCollector().ProcessExportRoots(oldMarkForeignRoots);
runtime/src/Heap/z/zGeneration.cpp:1368:    collector.MergeResurrectExportObjects(Generation::Old);
runtime/src/Heap/z/zGeneration.cpp:1369:    collector.PostResolveCycleTask();
runtime/src/Heap/z/zCrossVM.cpp:89:CrossRefHandler HeapGcState::GetCrossRefHandler(BaseObject *foreignProxy)
runtime/src/Heap/z/zCrossVM.cpp:92:    if (cycleRefHandlerForTest != nullptr) {
runtime/src/Heap/z/zCrossVM.cpp:93:        return cycleRefHandlerForTest;
runtime/src/Heap/z/zCrossVM.cpp:99:void HeapGcState::ResolveCycleRef()
runtime/src/Heap/z/zCrossVM.cpp:106:    std::unique_lock<std::mutex> resolverLock(cycleResolverMtx, std::try_to_lock);
runtime/src/Heap/z/zCrossVM.cpp:112:    std::unique_lock<std::mutex> cycleLock(cycleWorkStackMtx, std::try_to_lock);
runtime/src/Heap/z/zCrossVM.cpp:121:        auto it = cycleRefWorkStack.begin();
runtime/src/Heap/z/zCrossVM.cpp:122:        while (it != cycleRefWorkStack.end()) {
runtime/src/Heap/z/zCrossVM.cpp:131:                resurrectedExportObjectes.find(candidate) != resurrectedExportObjectes.end() ||
runtime/src/Heap/z/zCrossVM.cpp:132:                resurrectedExportObjectesForwardPhase.find(candidate) !=
runtime/src/Heap/z/zCrossVM.cpp:133:                    resurrectedExportObjectesForwardPhase.end()) {
runtime/src/Heap/z/zCrossVM.cpp:134:                cycleRefProgress.erase(candidateId);
runtime/src/Heap/z/zCrossVM.cpp:135:                it = cycleRefWorkStack.erase(it);
runtime/src/Heap/z/zCrossVM.cpp:140:        if (it == cycleRefWorkStack.end()) {
runtime/src/Heap/z/zCrossVM.cpp:153:        size_t externIndex = cycleRefProgress[id];
runtime/src/Heap/z/zCrossVM.cpp:159:            it = std::find_if(cycleRefWorkStack.begin(), cycleRefWorkStack.end(),
runtime/src/Heap/z/zCrossVM.cpp:163:            if (it == cycleRefWorkStack.end() || externIndex >= it->second.size()) {
runtime/src/Heap/z/zCrossVM.cpp:176:            auto resolveHook = GetCrossRefHandler(externObj);
runtime/src/Heap/z/zCrossVM.cpp:180:            // the callback runs. cycleResolverMtx alone prevents duplicate
runtime/src/Heap/z/zCrossVM.cpp:188:            if (cycleRefHandlerForTest != nullptr) {
runtime/src/Heap/z/zCrossVM.cpp:203:            cycleRefProgress[id] = externIndex;
runtime/src/Heap/z/zCrossVM.cpp:208:        cycleRefProgress.erase(id);
runtime/src/Heap/z/zCrossVM.cpp:213:    resurrectedExportObjectes.clear();
runtime/src/Heap/z/zCrossVM.cpp:214:    resurrectedExportObjectesForwardPhase.clear();
runtime/src/Heap/z/zCrossVM.cpp:217:void HeapGcState::PostResolveCycleTask()
runtime/src/Heap/z/zCrossVM.cpp:220:    if (cycleRefWorkStack.empty()) {
runtime/src/Heap/z/zRelocationSet.cpp:65:    ObserveExportOwnershipForTest(false);
runtime/src/Heap/z/zRelocationSet.cpp:67:    PrepareCycleRef();
runtime/src/Heap/z/zRelocationSet.cpp:69:    ObserveExportOwnershipForTest(true);
runtime/src/Heap/z/zMark.cpp:235:void HeapGcState::VisitMinorValueRoots(const std::function<void(BaseObject*)>& visitor)
runtime/src/Heap/z/zMark.cpp:238:        std::lock_guard<std::mutex> lock(resurrectExportMtx);
runtime/src/Heap/z/zMark.cpp:239:        CurrentizeValueRootSet(resurrectedExportObjectes, Generation::Young);
runtime/src/Heap/z/zMark.cpp:240:        CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase, Generation::Young);
runtime/src/Heap/z/zMark.cpp:242:        for (BaseObject* object : resurrectedExportObjectes) {
runtime/src/Heap/z/zMark.cpp:246:        for (BaseObject* object : resurrectedExportObjectesForwardPhase) {
runtime/src/Heap/z/zMark.cpp:250:    std::lock_guard<std::mutex> lock(cycleWorkStackMtx);
runtime/src/Heap/z/zMark.cpp:251:    CurrentizeValueRootMap(cycleRefWorkStack, Generation::Young);
runtime/src/Heap/z/zMark.cpp:253:    for (const auto& entry : cycleRefWorkStack) {
runtime/src/Heap/z/zMark.cpp:374:        VisitSurrectedExportRoots([&](BaseObject* object) { MarkOldObjectIfActive(object); });
runtime/src/Heap/z/zMark.cpp:437:        VisitMinorValueRoots(visitor);
runtime/src/Heap/z/zMark.cpp:465:        // should yield to the TLS tag set by VisitMinorRootSlots/ValueRoots.
runtime/src/Heap/z/zMark.cpp:883:void HeapGcState::ProcessExportRoots(WorkStack& foreignRootsSet)
runtime/src/Heap/z/zMark.cpp:896:            std::lock_guard<std::mutex> lock(externMtx);
runtime/src/Heap/z/zMark.cpp:898:            if (!discoveredExternObjects.emplace(exportObj, ValueRootList{}).second) {
runtime/src/Heap/z/zMark.cpp:919:                std::lock_guard<std::mutex> lock(externMtx);
runtime/src/Heap/z/zMark.cpp:920:                discoveredExternObjects[exportObj].push_back(object);
runtime/src/Heap/z/zMark.cpp:934:void HeapGcState::FindUselessExternObjects()
runtime/src/Heap/z/zMark.cpp:936:    std::lock_guard<std::mutex> lock(externMtx);
runtime/src/Heap/z/zMark.cpp:937:    CurrentizeValueRootMap(discoveredExternObjects, Generation::Old);
runtime/src/Heap/z/zMark.hpp:309:struct ValueRoot {
runtime/src/Heap/z/zMark.hpp:314:    ValueRoot(BaseObject* value, ForwardingStage source = ForwardingStage::OverwritePrevious)
runtime/src/Heap/z/zMark.hpp:328:struct ValueRootHash {
runtime/src/Heap/z/zMark.hpp:329:    size_t operator()(const ValueRoot& root) const { return std::hash<BaseObject*>{}(root.object); }
runtime/src/Heap/z/zMark.hpp:331:using ValueRootSet = std::unordered_set<ValueRoot, ValueRootHash>;
runtime/src/Heap/z/zMark.hpp:332:using ValueRootList = std::list<ValueRoot>;
runtime/src/Heap/z/zMark.hpp:333:using ValueRootMap = std::unordered_map<ValueRoot, ValueRootList, ValueRootHash>;
runtime/src/Heap/z/zMark.hpp:392:    static std::function<void(const ExportOwnershipTestObservation&)> testExportOwnershipResult;
runtime/src/Heap/z/zMark.hpp:409:    void ResurrectExportObject(BaseObject* obj)
runtime/src/Heap/z/zMark.hpp:413:        std::lock_guard<std::mutex> lg(resurrectExportMtx);
runtime/src/Heap/z/zMark.hpp:415:            resurrectedExportObjectes.erase(obj);
runtime/src/Heap/z/zMark.hpp:416:            resurrectedExportObjectes.insert(ValueRoot(ResolveCurrentValueRoot(
runtime/src/Heap/z/zMark.hpp:417:                obj, &resurrectedExportObjectes, Heap::GetHeap().ObjectGeneration(obj), ForwardingStage::IncomingNew),
runtime/src/Heap/z/zMark.hpp:420:            resurrectedExportObjectesForwardPhase.erase(obj);
runtime/src/Heap/z/zMark.hpp:421:            resurrectedExportObjectesForwardPhase.insert(ValueRoot(
runtime/src/Heap/z/zMark.hpp:422:                ResolveCurrentValueRoot(
runtime/src/Heap/z/zMark.hpp:423:                    obj, &resurrectedExportObjectesForwardPhase, Heap::GetHeap().ObjectGeneration(obj), ForwardingStage::IncomingNew),
runtime/src/Heap/z/zMark.hpp:428:    void VisitAllResurrectExportObjects(const RootVisitor& visitor)
runtime/src/Heap/z/zMark.hpp:430:        std::lock_guard<std::mutex> lg(resurrectExportMtx);
runtime/src/Heap/z/zMark.hpp:431:        for (auto& obj : resurrectedExportObjectes) {
runtime/src/Heap/z/zMark.hpp:478:    std::mutex externMtx;
runtime/src/Heap/z/zMark.hpp:479:    ValueRootMap discoveredExternObjects;
runtime/src/Heap/z/zMark.hpp:481:    void ObserveExportOwnershipForTest(bool afterHandoff);
runtime/src/Heap/z/zMark.hpp:486:    std::mutex cycleResolverMtx;
runtime/src/Heap/z/zMark.hpp:487:    std::mutex cycleWorkStackMtx;
runtime/src/Heap/z/zMark.hpp:488:    ValueRootMap cycleRefWorkStack;
runtime/src/Heap/z/zMark.hpp:492:    // Protected by cycleWorkStackMtx together with the root carrier.
runtime/src/Heap/z/zMark.hpp:493:    std::unordered_map<U32, size_t> cycleRefProgress;
runtime/src/Heap/z/zMark.hpp:494:    std::mutex resurrectExportMtx;
runtime/src/Heap/z/zMark.hpp:495:    ValueRootSet resurrectedExportObjectes;
runtime/src/Heap/z/zMark.hpp:496:    ValueRootSet resurrectedExportObjectesForwardPhase;
runtime/src/Heap/z/zMark.hpp:501:    BaseObject* ResolveCurrentValueRoot(BaseObject* value, const void* owner, Generation generation,
runtime/src/Heap/z/zMark.hpp:503:    void CurrentizeValueRootSet(ValueRootSet& roots, Generation generation) const;
runtime/src/Heap/z/zMark.hpp:504:    void CurrentizeValueRootMap(ValueRootMap& roots, Generation generation) const;
runtime/src/Heap/z/zMark.hpp:528:    void ProcessExportRoots(WorkStack& foreignRootsSet);
runtime/src/Heap/z/zMark.hpp:532:    void FindUselessExternObjects();
runtime/src/Heap/z/zMark.hpp:536:    void VisitSurrectedExportRoots(const std::function<void(BaseObject*)>& visitor);
runtime/src/Heap/z/zMark.hpp:569:    void SetCycleRefHandlerForTest(CrossRefHandler handler) { cycleRefHandlerForTest = handler; }
runtime/src/Heap/z/zMark.hpp:594:    void PostResolveCycleTask();
runtime/src/Heap/z/zMark.hpp:595:    void PrepareCycleRef()
runtime/src/Heap/z/zMark.hpp:597:        std::lock_guard<std::mutex> lg(cycleWorkStackMtx);
runtime/src/Heap/z/zMark.hpp:598:        CurrentizeValueRootMap(cycleRefWorkStack, Generation::Old);
runtime/src/Heap/z/zMark.hpp:599:        CurrentizeValueRootMap(discoveredExternObjects, Generation::Old);
runtime/src/Heap/z/zMark.hpp:600:        for (auto& entry : discoveredExternObjects) {
runtime/src/Heap/z/zMark.hpp:601:            ValueRootList& destination = cycleRefWorkStack[entry.first];
runtime/src/Heap/z/zMark.hpp:604:        discoveredExternObjects.clear();
runtime/src/Heap/z/zMark.hpp:606:    void MergeResurrectExportObjects(Generation generation)
runtime/src/Heap/z/zMark.hpp:608:        std::lock_guard<std::mutex> lg(resurrectExportMtx);
runtime/src/Heap/z/zMark.hpp:609:        CurrentizeValueRootSet(resurrectedExportObjectes, generation);
runtime/src/Heap/z/zMark.hpp:610:        CurrentizeValueRootSet(resurrectedExportObjectesForwardPhase, generation);
runtime/src/Heap/z/zMark.hpp:611:        resurrectedExportObjectes.insert(resurrectedExportObjectesForwardPhase.begin(),
runtime/src/Heap/z/zMark.hpp:612:            resurrectedExportObjectesForwardPhase.end());
runtime/src/Heap/z/zMark.hpp:613:        resurrectedExportObjectesForwardPhase.clear();
runtime/src/Heap/z/zMark.hpp:705:    void ResolveCycleRef();
runtime/src/Heap/z/zMark.hpp:998:    void VisitMinorValueRoots(const std::function<void(BaseObject*)>& visitor);
runtime/src/Heap/z/zMark.hpp:1055:    void PreforwardDiscoveredExternObjects(Generation generation);
runtime/src/Heap/z/zMark.hpp:1056:    void PreforwardAllResurrectExportFromObjects(Generation generation);
runtime/src/Heap/z/zMark.hpp:1057:    CrossRefHandler GetCrossRefHandler(BaseObject* foreignProxy);
runtime/src/Heap/z/zMark.hpp:1059:    CrossRefHandler cycleRefHandlerForTest = nullptr;
runtime/src/Heap/z/zTracing.cpp:33:std::function<void(const ExportOwnershipTestObservation&)> HeapGcState::testExportOwnershipResult;
runtime/src/Heap/z/zHeap.cpp:172:void Heap::ResolveCycleRef() { GetCollector().ResolveCycleRef(); }
runtime/src/Heap/z/zHeap.cpp:458:    reinterpret_cast<HeapGcState&>(GetCollector()).ResurrectExportObject(recordObj);
runtime/src/Heap/z/zExportOwnershipTestObservations.hpp:8:void HeapGcState::ObserveExportOwnershipForTest(bool afterHandoff)
runtime/src/Heap/z/zExportOwnershipTestObservations.hpp:10:    if (!testExportOwnershipResult) {
runtime/src/Heap/z/zExportOwnershipTestObservations.hpp:16:        std::lock_guard<std::mutex> lock(externMtx);
runtime/src/Heap/z/zExportOwnershipTestObservations.hpp:17:        observation.discoveredOwners = discoveredExternObjects.size();
runtime/src/Heap/z/zExportOwnershipTestObservations.hpp:18:        for (const auto& owner : discoveredExternObjects) {
runtime/src/Heap/z/zExportOwnershipTestObservations.hpp:25:        std::lock_guard<std::mutex> lock(cycleWorkStackMtx);
runtime/src/Heap/z/zExportOwnershipTestObservations.hpp:26:        observation.handoffOwners = cycleRefWorkStack.size();
runtime/src/Heap/z/zExportOwnershipTestObservations.hpp:27:        for (const auto& owner : cycleRefWorkStack) {
runtime/src/Heap/z/zExportOwnershipTestObservations.hpp:34:    testExportOwnershipResult(observation);
```
