# Barrier / raw-value fold plan (read-only)
待主控登记进 `/root/cj_build/ops/CURRENT_DOCS.manifest`（若采纳为当前文档）。
Coordinates are active candidate worktree, HEAD f49bd78712f5cd31043a1456f85292050f3bc8ce; other agents may advance line numbers. No product edits by this planning task.

## Minimal cuts, ordered

| Cut | Concrete implementation | Mandatory caller conversion / caveat |
|---|---|---|
| 1. Delete duplicated color predicates | HeapGcState::IsLoadBad -> `ZPointer::is_load_bad(slot.GetFieldValue())` if API exists, otherwise same mask expression at the consuming real barrier; remap_generation -> existing ZBarrier::remap_generation(zpointer) | zRelocate.cpp:152-157 and header make_load_good are real callers. Existing ZBarrier returns ZGeneration*, old helper returns ZGenerationId: call returned generation directly, derive id only when the receiving API truly requires it. Do not add a second enum-returning routing implementation. |
| 2. Single colored-word resolve | Remove HeapGcState::make_load_good(RefField&, provenance); use existing ZBarrier::make_load_good(zpointer) at GetAndTryTagObj and relocation consumers | Current canonical path: zBarrier.inline.hpp:253 -> relocate_or_remap(:239) -> Heap::relocate_or_remap_object -> ZGeneration. Old overload passes ForwardingProvenance whereas canonical path currently does not: explicitly decide preservation through the real downstream entry (one overload delegating same funnel is acceptable only if needed for infrastructure diagnostics), do not silently discard recorded provenance or clone routing. Preserve null/nonheap behavior and resolved-once rule. |
| 3. Current-value verification | Move ValidateCurrentValue, JudgeHandOutTarget, FailClosedLoad and CheckStoreGoodTarget to ZBarrier/raw boundary helpers (state-free static) | Current bodies zCollectedHeap.cpp:275/311/324 and zMark.hpp:641. These validate raw BaseObject/header state, not colored-word remap; never replace with a blind `make_load_good` on invented color. Consumers include ZCrossVM current roots, ZRelocate fail-closed exits, direct tests. Preserve fail-closed target diagnostics. |
| 4. Pure color construction | GetAndTryTagRefField / WithProvenance -> one ZBarrier raw-current-value-to-store-good boundary | Current body zMark.hpp:708/714 calls validation, then ZAddress::store_good; it must NOT call ResolveStoreValue again. Keep null and nonheap metadata handling. HeapSlot private BaseObject constructor friend currently HeapGcState: use public zpointer constructor for null or transfer friend only if necessary; no need for generic collector friendship. CheckStoreGoodTarget belongs in same funnel. |
| 5. Raw historical value resolution | ResolveStoreValue + IsAlreadyToStoreValue -> ZRelocate raw-value infrastructure entry with explicit generation/provenance | zRelocate.cpp:1255 loop has current compact-destination identity, forwarding-receipt, external/nonheap, missing-identity and fail-closed branches. It is not interchangeable with ZBarrier::make_load_good(zpointer), because raw inputs lack source color. Move full body first, route existing generation forwarding/relocation authorities directly; do not create a fabricated colored word or duplicate FindToVersion. This is a Cangjie raw carrier adapter that needs explicit exception table. |
| 6. Slot producer/consumer | GetAndTryTagObj + TryUpdateRefField / Impl + RootSlotWriteback overloads -> real ZBarrier heap field vs ZUncoloredRoot plain-root owner | Current GetAndTryTagObj in zMark.cpp:130 observes exact old colored word, remaps, validates, recolors and CASes expected observed word. Preserve strong/weak routing, successful/failed CAS handling and plain-root ABI. Do not generalize heap-field and root writes to one runtime kind switch. RefSlotKind remaining use should follow true strong/weak closures. |
| 7. Remove dead FindLatestVersion | Search presently yields only declaration zMark.hpp:325 and definition zCollectedHeap.cpp:285 | Mechanical product+test search in appendix is positive definition-only baseline, not proof of semantic equivalence. Delete with paired grep evidence; do not move a dead helper into ZRelocate. |

Existing canonical routing is already duplicated byte-for-byte in the class: ZBarrier::remap_generation zBarrier.inline.hpp:217-236 and HeapGcState version zMark.hpp:529-548 select old-good -> young, young-good -> old, remembered -> old, otherwise young forwarding membership. The correct cut is consumer reroute and deletion, not another renamed routine.

## Runtime entry chains

- Normal field/atomic barrier: ZBarrierSet.cpp:43 / zBarrier.cpp:775 -> canonical ZBarrier::make_load_good(zpointer) -> Heap::relocate_or_remap_object -> actual ZGeneration relocation. Existing canonical path must remain sole color-based routing point.
- GC old foreign ownership traversal: ZCrossVM::ProcessExportRoots -> HeapGcState::GetAndTryTagObj -> make_load_good -> GetAndTryTagRefField -> observed-word CAS. Replace dependency with actual ZBarrier field helper while preserving weak-edge discovery semantics.
- GC root/remset relocation: zRelocate.cpp RemapYoungRoots / sibling preforward callbacks -> make_load_good or ResolveStoreValue -> current value -> RootSlotWriteback/colored CompareExchange. These paths are not all mutator barriers, so testing only ZBarrierSet fast path misses them.
- Cross-VM raw containers: ZCrossVM::ResolveCurrentValueRoot -> ValidateCurrentValue for IncomingNew, otherwise receipt/ResolveStoreValue -> rebuild owner map/set -> root visitor. Same raw identity must not be remapped twice.

## Counters / dead probes

GetAndTryTagRefField currently also owns kColourWhoProbe, NoteStoreGoodOnBadTarget, colourWhoTotal/Bad. Search exact producer+consumer before migration. If disabled compile-time and diagnostics registry says obsolete, delete as a separate evidenced dead-path cut; otherwise counters belong to actual raw-current-to-store-good boundary, never to a new collector object. Do not silently lose diagnostic state by converting const instance methods to statics.

## Test and ELF binding changes

Affected direct-call test files for queried methods: test_partial_array.cpp (StoreTarget), test_remset.cpp, clear_entries_product_unit.cpp. Full expanded call inventory below also includes TryUpdateRefField / RootSlotWriteback and generation entry consumers; use it rather than this short list for final change set.

Most important explicit product binding: clear_entries_product_unit.cpp:165-181 `ProductRelocateOrRemap` still requests
`_ZNK12MapleRuntime11HeapGcState24relocate_or_remap_objectEPNS_10BaseObjectENS_13ZGenerationIdE`
and casts to `BaseObject* (*)(const HeapGcState*, BaseObject*, ZGenerationId)` although current canonical product owner is Heap. Change both mangled name AND first receiver type/address to the actual Heap member, retain dlopen RTLD_NOLOAD + dladdr libcangjie-runtime.so assertions. Determine exact new symbol from full nm defined-only on built product rather than guessing. This existing binding is already stale relative to latest owner migration, so flag to parent immediately.

run_standalone.sh:469 onwards checks forbidden shadow definitions (includes historical WCollector/Collector symbol strings); these are negative lineage guards, not live old hierarchy consumers. Preserve purpose, add/check actual new entry symbols as needed; do not blindly replace strings and accidentally remove baseline shadow detection. Test manifest product_call_manifest_loadheal.tsv refers HeapGcState::RemapYoungRoots; migrate only if that phase owner actually changes, not merely because barrier helpers change.

## Validation obligations

Use same product SO default/testable, ELF lineage and explicit real-entry dlsym binding. Prior red-arm burden splits into color resolution, raw-root identity, observed-CAS writeback and current-value validation. A synthetic helper test cannot prove the phase consumer. Preserve existing overlap/compact-destination and weak-field assertions; do not lower assertions to permit new routing. New naming must come from ZGC corresponding function for color paths; raw carrier adaptation explicitly documented as infrastructure difference.

## Complete query inventory

Query regex: `IsLoadBad|make_load_good\(|remap_generation\(|ValidateCurrentValue|ResolveStoreValue|GetAndTryTagRefField|GetAndTryTagObj|FindLatestVersion|JudgeHandOutTarget|FailClosedLoad|CheckStoreGoodTarget|IsAlreadyToStoreValue|RootSlotWriteback|TryUpdateRefField|relocate_or_remap_object`; scopes runtime/src and runtime/tests/gc_unit; rc=0. Definitions/comments are retained and must not be mistaken for live calls.

```text
runtime/tests/gc_unit/test_partial_array.cpp:65:        const RefField<> coloured = collector.GetAndTryTagRefField(target);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:105:        return collector.GetAndTryTagRefField(value);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:108:    static BaseObject* ResolveStoreValue(HeapGcState& collector, BaseObject* value)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:111:        return collector.ResolveStoreValue(value, provenance, Generation::Old);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:114:    static void CheckStoreGoodTarget(HeapGcState& collector, BaseObject* value)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:116:        collector.CheckStoreGoodTarget("ForwardingLookupWitness", value,
runtime/tests/gc_unit/clear_entries_product_unit.cpp:127:        BaseObject* mapped = Heap::GetHeap().old().relocate_or_remap_object(oldObj);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:144:        return Heap::GetHeap().old().relocate_or_remap_object(object);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:155:    static bool TryUpdateRefField(HeapGcState& collector, BaseObject* obj, RefField<>& field, BaseObject*& newRef)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:157:        return collector.TryUpdateRefField(obj, field, newRef);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:173:            "_ZNK12MapleRuntime11HeapGcState24relocate_or_remap_objectEPNS_10BaseObjectENS_13ZGenerationIdE");
runtime/tests/gc_unit/clear_entries_product_unit.cpp:938:GC_TEST(ForwardingPublicationProduct, ResolveStoreValueSafeAddrAfterForwardingTableGone)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:955:    BaseObject* resolved = RelocationReceiptTestAccess::ResolveStoreValue(collector, liveObject);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:1428:// product ResolveStoreValue entry; the only legal result is fail-closed.
runtime/tests/gc_unit/clear_entries_product_unit.cpp:1641:GC_TEST(ForwardingPublicationProduct, ResolveStoreValueFollowsForwardedDestination)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:1683:    BaseObject* resolved = RelocationReceiptTestAccess::ResolveStoreValue(collector, first);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:1685:    GC_EXPECT_TRUE(HeapGcState::JudgeHandOutTarget(resolved) == HandVerdict::Usable);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2698:GC_TEST(ForwardingPublicationProduct, ResolveStoreValueAlreadyToStartRejectsNonUsable)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2714:        (void)RelocationReceiptTestAccess::ResolveStoreValue(collector, compactedStart);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2724:GC_TEST(ForwardingPublicationProduct, ResolveStoreValueAlreadyToStartWithUsableTarget)
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2739:    BaseObject* resolved = RelocationReceiptTestAccess::ResolveStoreValue(collector, compactedStart);
runtime/tests/gc_unit/clear_entries_product_unit.cpp:2741:    GC_EXPECT_TRUE(HeapGcState::JudgeHandOutTarget(resolved) == HandVerdict::Usable);
runtime/tests/gc_unit/test_trustp1_phase1.cpp:29:GC_TEST(TrustP1, RootSlotWritebackPlainIsPlain)
runtime/tests/gc_unit/test_remset.cpp:70:        return collector.GetAndTryTagRefField(object);
runtime/tests/gc_unit/test_defect_regressions.cpp:156:// Product predicate: Heap::IsHeapAddress; writeback shape RootSlotWriteback keeps target.
runtime/tests/gc_unit/run_forwarding_p2_arms.py:45:    '            Collector::FailClosedLoad(\n'
runtime/src/CompilerCalls.cpp:905:    current = heap.relocate_or_remap_object(current, id);
runtime/src/Heap/z/zHeap.hpp:98:    BaseObject* relocate_or_remap_object(BaseObject* object, ZGenerationId generation);
runtime/src/Heap/z/zHeap.hpp:99:    BaseObject* make_load_good(RefField<>& ref, const ForwardingProvenance& provenance);
runtime/src/Heap/z/zMark.cpp:106:    BaseObject* latest = make_load_good(oldField, provenance);
runtime/src/Heap/z/zMark.cpp:114:    // plainroots only applies to stack/reg ObjectRef slots (RootSlotWriteback via !IsHeapAddress).
runtime/src/Heap/z/zMark.cpp:115:    RefField<> newField = GetAndTryTagRefField(latest);
runtime/src/Heap/z/zMark.cpp:130:BaseObject* HeapGcState::GetAndTryTagObj(RefSlotKind kind, BaseObject* obj, RefField<>& field)
runtime/src/Heap/z/zMark.cpp:147:    latest = make_load_good(oldField, provenance);
runtime/src/Heap/z/zMark.cpp:154:    RefField<> newField = GetAndTryTagRefField(latest);
runtime/src/Heap/z/zMark.cpp:221:    object = ValidateCurrentValue(object, provenance);
runtime/src/Heap/z/zMark.cpp:234:    BaseObject* referent = GetAndTryTagObj(RefSlotKind::WEAK_REFERENT, reference, referentField);
runtime/src/Heap/z/zMark.cpp:525:    HeapGcState::FailClosedLoad(
runtime/src/Heap/z/zUncoloredRoot.hpp:14:    static zaddress make_load_good(zaddress_unsafe addr, uintptr_t color);
runtime/src/Heap/z/zUncoloredRoot.inline.hpp:17:    const zaddress loadGood = make_load_good(addr, color);
runtime/src/Heap/z/zUncoloredRoot.inline.hpp:22:inline zaddress ZUncoloredRoot::make_load_good(zaddress_unsafe addr, uintptr_t color)
runtime/src/Heap/z/zUncoloredRoot.inline.hpp:26:        return ZBarrier::relocate_or_remap(addr, ZBarrier::remap_generation(colorPtr));
runtime/src/Heap/z/zCollectedHeap.cpp:275:BaseObject* HeapGcState::ValidateCurrentValue(BaseObject* ref, const ForwardingProvenance& provenance) const
runtime/src/Heap/z/zCollectedHeap.cpp:277:    if (ref == nullptr || !Heap::IsHeapAddress(ref) || JudgeHandOutTarget(ref) == HandVerdict::Usable) {
runtime/src/Heap/z/zCollectedHeap.cpp:280:    FailClosedLoad("current raw value required", ref, 0, provenance);
runtime/src/Heap/z/zCollectedHeap.cpp:285:BaseObject* HeapGcState::FindLatestVersion(BaseObject* obj, const ForwardingProvenance& provenance, Generation generation) const
runtime/src/Heap/z/zCollectedHeap.cpp:291:    BaseObject* to = FindToVersion(obj, generation).GetOrFailClosed("HeapGcState::FindLatestVersion", provenance);
runtime/src/Heap/z/zCollectedHeap.cpp:295:                         "FindLatestVersion: route dest %p has no tip and from %p is not valid",
runtime/src/Heap/z/zCollectedHeap.cpp:302:                 "FindLatestVersion: no to-version for invalid from-object %p "
runtime/src/Heap/z/zCollectedHeap.cpp:311:HandVerdict HeapGcState::JudgeHandOutTarget(BaseObject* target)
runtime/src/Heap/z/zCollectedHeap.cpp:324:[[noreturn]] void HeapGcState::FailClosedLoad(const char* site, BaseObject* target, uintptr_t slotBits,
runtime/src/Heap/z/zCollectedHeap.cpp:327:    const HandVerdict verdict = JudgeHandOutTarget(target);
runtime/src/Heap/z/zCrossVM.hpp:94:    // their RootObligation on the existing ResolveStoreValue authority and
runtime/src/Heap/z/zStringDedup.cpp:65:    BaseObject* object = Heap::GetHeap().make_load_good(reference, {});
runtime/src/Heap/z/zGeneration.cpp:1506:BaseObject* ZGeneration::relocate_or_remap_object(BaseObject* object)
runtime/src/Heap/z/zGeneration.cpp:1508:    return relocate_or_remap_object(object,
runtime/src/Heap/z/zGeneration.cpp:1512:BaseObject* ZGeneration::relocate_or_remap_object(BaseObject* object,
runtime/src/Heap/z/zRelocate.cpp:138:void HeapGcState::CheckStoreGoodTarget(const char* consumer, BaseObject* target,
runtime/src/Heap/z/zRelocate.cpp:144:    (void)ValidateCurrentValue(target, provenance);
runtime/src/Heap/z/zRelocate.cpp:148:bool HeapGcState::TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& field, BaseObject*& fromObj,
runtime/src/Heap/z/zRelocate.cpp:152:    if (IsLoadBad(oldRef)) {
runtime/src/Heap/z/zRelocate.cpp:155:            toObj = ZGeneration::generation(remap_generation(oldRef))->relocate_or_remap_object(fromObj);
runtime/src/Heap/z/zRelocate.cpp:157:            toObj = FindToVersion(fromObj, static_cast<Generation>(remap_generation(oldRef))).GetOrFailClosed(
runtime/src/Heap/z/zRelocate.cpp:158:                "HeapGcState::TryUpdateRefFieldImpl", provenance);
runtime/src/Heap/z/zRelocate.cpp:164:        // expected 仍是 observed-raw（oldRef.GetFieldValue()）；模板 = GetAndTryTagRefField。
runtime/src/Heap/z/zRelocate.cpp:165:        RefField<> tmpField = GetAndTryTagRefField(toObj);
runtime/src/Heap/z/zRelocate.cpp:191:bool HeapGcState::TryUpdateRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const
runtime/src/Heap/z/zRelocate.cpp:195:    return TryUpdateRefFieldImpl<false>(obj, field, oldRef, newRef, provenance);
runtime/src/Heap/z/zRelocate.cpp:220:            (void)ZGeneration::generation(id)->relocate_or_remap_object(to_object(safe(observed)));
runtime/src/Heap/z/zRelocate.cpp:297:                (void)ZGeneration::old()->relocate_or_remap_object(oldObj);
runtime/src/Heap/z/zRelocate.cpp:477:// On CAS fail, accept the peer's update (major TryUpdateRefFieldImpl shape).
runtime/src/Heap/z/zRelocate.cpp:485:        CHECK_DETAIL(HeapGcState::JudgeHandOutTarget(object) == HandVerdict::Usable,
runtime/src/Heap/z/zRelocate.cpp:526:    BaseObject* resolved = make_load_good(observed, provenance);
runtime/src/Heap/z/zRelocate.cpp:529:    CHECK_DETAIL(HeapGcState::JudgeHandOutTarget(resolved) == HandVerdict::Usable,
runtime/src/Heap/z/zRelocate.cpp:547:    BaseObject* resolved = ResolveStoreValue(from, provenance, Generation::Young);
runtime/src/Heap/z/zRelocate.cpp:550:    CHECK_DETAIL(HeapGcState::JudgeHandOutTarget(resolved) == HandVerdict::Usable,
runtime/src/Heap/z/zRelocate.cpp:559:    // N1: major-style CAS tolerate (TryUpdateRefFieldImpl family). Under multi-worker
runtime/src/Heap/z/zRelocate.cpp:582:        BaseObject* resolvedBase = ResolveStoreValue(knownBase, provenance, Generation::Young);
runtime/src/Heap/z/zRelocate.cpp:584:                         HeapGcState::JudgeHandOutTarget(resolvedBase) == HandVerdict::Usable,
runtime/src/Heap/z/zRelocate.cpp:633:        HeapGcState::FailClosedLoad(
runtime/src/Heap/z/zRelocate.cpp:640:    RefField<> newField = RootSlotWriteback(current, field);
runtime/src/Heap/z/zRelocate.cpp:709:        HeapGcState::FailClosedLoad(
runtime/src/Heap/z/zRelocate.cpp:973:    BaseObject* target = collector.make_load_good(value, provenance);
runtime/src/Heap/z/zRelocate.cpp:1255:BaseObject* HeapGcState::ResolveStoreValue(BaseObject* ref, const ForwardingProvenance& provenance,
runtime/src/Heap/z/zRelocate.cpp:1267:        if (IsAlreadyToStoreValue(current, generation)) {
runtime/src/Heap/z/zRelocate.cpp:1273:            HeapGcState::JudgeHandOutTarget(current) == HandVerdict::Usable) {
runtime/src/Heap/z/zRelocate.cpp:1299:            if (HeapGcState::JudgeHandOutTarget(current) == HandVerdict::Usable) {
runtime/src/Heap/z/zRelocate.cpp:1310:            const HandVerdict verdict = HeapGcState::JudgeHandOutTarget(to);
runtime/src/Heap/z/zRelocate.cpp:1333:                    HeapGcState::JudgeHandOutTarget(current) == HandVerdict::Usable) {
runtime/src/Heap/z/zRelocate.cpp:1338:                HeapGcState::JudgeHandOutTarget(current) == HandVerdict::Usable &&
runtime/src/Heap/z/zRelocate.cpp:1344:                "[FWDTABLE][resolve-miss] site=no-forwarding consumer=HeapGcState::ResolveStoreValue "
runtime/src/Heap/z/zRelocate.cpp:1370:                static_cast<unsigned>(HeapGcState::JudgeHandOutTarget(current)));
runtime/src/Heap/z/zRelocate.cpp:1371:            FailClosedLoad("HeapGcState::ResolveStoreValue.no-forwarding", current, 0, provenance);
runtime/src/Heap/z/zRelocate.cpp:1378:            HeapGcState::JudgeHandOutTarget(current) == HandVerdict::Usable) {
runtime/src/Heap/z/zRelocate.cpp:1381:        BaseObject* resolved = ZGeneration::generation(static_cast<ZGenerationId>(generation))->relocate_or_remap_object(current, provenance);
runtime/src/Heap/z/zRelocate.cpp:1383:            FailClosedLoad("HeapGcState::ResolveStoreValue.unresolved", current, 0, provenance);
runtime/src/Heap/z/zRelocate.cpp:1391:                HeapGcState::JudgeHandOutTarget(current) == HandVerdict::Usable) {
runtime/src/Heap/z/zRelocate.cpp:1397:            if (HeapGcState::JudgeHandOutTarget(current) == HandVerdict::Usable &&
runtime/src/Heap/z/zRelocate.cpp:1402:            FailClosedLoad("HeapGcState::ResolveStoreValue.missing-identity", current, 0, provenance);
runtime/src/Heap/z/zRelocate.cpp:1410:    BaseObject* to = ZGeneration::generation(static_cast<ZGenerationId>(generation))->relocate_or_remap_object(obj);
runtime/src/Heap/z/zRelocate.cpp:2185:            //     replayed into the remembered set and then refused at ResolveStoreValue
runtime/src/Heap/z/zRelocate.cpp:2657:        HeapGcState::FailClosedLoad("ZRelocate::forward_object requires a forwarding entry", object, 0, provenance);
runtime/src/Heap/z/zStoreBarrierBuffer.cpp:58:        ZGeneration* generation = ZBarrier::remap_generation(ptr);
runtime/src/Heap/z/zStoreBarrierBuffer.cpp:88:    const zaddress remapped = ZBarrier::make_load_good(colored);
runtime/src/Heap/z/zStoreBarrierBuffer.cpp:138:        const zaddress addr = ZBarrier::make_load_good(entry.prev);
runtime/src/Heap/z/zStoreBarrierBuffer.cpp:163:        const zaddress addr = ZBarrier::make_load_good(entry.prev);
runtime/src/Heap/z/zMark.hpp:309:    static HandVerdict JudgeHandOutTarget(BaseObject* target);
runtime/src/Heap/z/zMark.hpp:310:    [[noreturn]] static void FailClosedLoad(const char* site, BaseObject* target, uintptr_t slotBits,
runtime/src/Heap/z/zMark.hpp:312:    BaseObject* ValidateCurrentValue(BaseObject* ref, const ForwardingProvenance& provenance) const;
runtime/src/Heap/z/zMark.hpp:313:    bool IsLoadBad(RefField<>& ref) const
runtime/src/Heap/z/zMark.hpp:317:    BaseObject* make_load_good(RefField<>& ref, const ForwardingProvenance& provenance) const
runtime/src/Heap/z/zMark.hpp:323:        return ZGeneration::generation(remap_generation(ref))->relocate_or_remap_object(target, provenance);
runtime/src/Heap/z/zMark.hpp:325:    BaseObject* FindLatestVersion(BaseObject* obj, const ForwardingProvenance& provenance, Generation generation) const;
runtime/src/Heap/z/zMark.hpp:497:    BaseObject* GetAndTryTagObj(RefSlotKind kind, BaseObject* obj, RefField<>& field);
runtime/src/Heap/z/zMark.hpp:500:    BaseObject* ResolveStoreValue(BaseObject* ref, const ForwardingProvenance& provenance,
runtime/src/Heap/z/zMark.hpp:512:    // IsLoadBad is declared on HeapGcState (HeapGcState.h) so the six phase barriers, which hold a
runtime/src/Heap/z/zMark.hpp:529:    ZGenerationId remap_generation(RefField<>& ref) const
runtime/src/Heap/z/zMark.hpp:578:                obj = ValidateCurrentValue(obj, provenance);
runtime/src/Heap/z/zMark.hpp:594:    // ZGeneration::relocate_or_remap_object (zGeneration.inline.hpp).
runtime/src/Heap/z/zMark.hpp:641:    void CheckStoreGoodTarget(const char* consumer, BaseObject* target,
runtime/src/Heap/z/zMark.hpp:651:    bool TryUpdateRefField(BaseObject* obj, RefField<>& field, BaseObject*& newRef) const;
runtime/src/Heap/z/zMark.hpp:675:    // the address.  This is deliberately separate from GetAndTryTagRefField:
runtime/src/Heap/z/zMark.hpp:676:    // unclassified store values still have to pass ResolveStoreValue first.
runtime/src/Heap/z/zMark.hpp:697:    // GetAndTryTagRefField resolves again, and under in-place compaction that second resolve is
runtime/src/Heap/z/zMark.hpp:708:    RefField<> GetAndTryTagRefField(BaseObject* target) const
runtime/src/Heap/z/zMark.hpp:711:        return GetAndTryTagRefFieldWithProvenance(target, provenance);
runtime/src/Heap/z/zMark.hpp:714:    RefField<> GetAndTryTagRefFieldWithProvenance(BaseObject* target,
runtime/src/Heap/z/zMark.hpp:728:        // (zAddress.inline.hpp:609-624,806-811). ResolveStoreValue is our
runtime/src/Heap/z/zMark.hpp:731:        target = ValidateCurrentValue(target, provenance);
runtime/src/Heap/z/zMark.hpp:734:        CheckStoreGoodTarget("GetAndTryTagRefField", target, provenance);
runtime/src/Heap/z/zMark.hpp:796:    bool IsAlreadyToStoreValue(BaseObject* target, Generation generation) const
runtime/src/Heap/z/zMark.hpp:799:            HeapGcState::JudgeHandOutTarget(target) == HandVerdict::Usable &&
runtime/src/Heap/z/zMark.hpp:808:    RefField<> RootSlotWriteback(BaseObject* target, const RefField<>& /*slot*/) const
runtime/src/Heap/z/zMark.hpp:810:        return GetAndTryTagRefField(target);
runtime/src/Heap/z/zMark.hpp:895:    bool TryUpdateRefFieldImpl(BaseObject* obj, RefField<>& ref, BaseObject*& oldRef, BaseObject*& newRef,
runtime/src/Heap/z/zGeneration.hpp:139:    BaseObject* relocate_or_remap_object(BaseObject* object);
runtime/src/Heap/z/zGeneration.hpp:140:    BaseObject* relocate_or_remap_object(BaseObject* object, const ForwardingProvenance& provenance);
runtime/src/Heap/z/zBarrier.hpp:75:    static ZGeneration* remap_generation(zpointer ptr);
runtime/src/Heap/z/zBarrier.hpp:77:    static zaddress make_load_good(zpointer ptr);
runtime/src/Heap/z/zBarrierSet.cpp:43:    return ZBarrier::make_load_good(prev);
runtime/src/Heap/z/zCrossVM.cpp:359:                BaseObject* target = Heap::GetHeap().GetCollector().GetAndTryTagObj(HeapGcState::RefSlotKind::STRONG, object, field);
runtime/src/Heap/z/zCrossVM.cpp:382:        return Heap::GetHeap().GetCollector().ValidateCurrentValue(value, provenance);
runtime/src/Heap/z/zCrossVM.cpp:393:            : Heap::GetHeap().GetCollector().ResolveStoreValue(value, provenance, static_cast<Generation>(forwarding->table_generation()));
runtime/src/Heap/z/zCrossVM.cpp:397:    CHECK_DETAIL(HeapGcState::JudgeHandOutTarget(current) == HandVerdict::Usable,
runtime/src/Heap/z/zRemembered.cpp:159:            BaseObject* to = Heap::GetHeap().relocate_or_remap_object(
runtime/src/Heap/z/zBarrier.cpp:775:    mark_and_remember(reinterpret_cast<volatile zpointer*>(fieldAddress), make_load_good(prev));
runtime/src/Heap/z/zHeap.cpp:212:BaseObject* Heap::make_load_good(RefField<>& ref, const ForwardingProvenance& provenance)
runtime/src/Heap/z/zHeap.cpp:214:    return GetCollector().make_load_good(ref, provenance);
runtime/src/Heap/z/zHeap.cpp:259:BaseObject* Heap::relocate_or_remap_object(BaseObject* object, ZGenerationId generation)
runtime/src/Heap/z/zHeap.cpp:261:    return GetZGeneration(generation).relocate_or_remap_object(object);
runtime/src/Heap/z/zBarrier.inline.hpp:217:inline ZGeneration* ZBarrier::remap_generation(zpointer ptr)
runtime/src/Heap/z/zBarrier.inline.hpp:245:    return from_object(Heap::GetHeap().relocate_or_remap_object(to_object(safe(addr)), id));
runtime/src/Heap/z/zBarrier.inline.hpp:253:inline zaddress ZBarrier::make_load_good(zpointer ptr)
runtime/src/Heap/z/zBarrier.inline.hpp:262:                            remap_generation(ptr));
runtime/src/Heap/z/zBarrier.inline.hpp:273:    return remap(to_zaddress_unsafe(untype(RefField<>(ptr).GetTargetObject())), remap_generation(ptr));
runtime/src/Heap/z/zBarrier.inline.hpp:311:    const zaddress load_good_addr = make_load_good(o);
```
