// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "B3Root.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Base/Log.h"
#include "Collector/FinalizerProcessor.h"
#include "Collector/GcStats.h"
#include "Common/Runtime.h"
#include "Common/StackType.h"
#include "Concurrency/Concurrency.h"
#include "Heap/Allocator/RegionInfo.h"
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/Barrier/RememberedSet.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MFuncdesc.inline.h"
#include "ObjectModel/RefField.inline.h"
#include "StackMap/CompressedStackMap.h"
#include "StackMap/StackMap.h"
#include "UnwindStack/GcStackInfo.h"

namespace MapleRuntime {
namespace {

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

// Fixed open-addressing table: slot identity → value_then at concurrent enum.
// Capacity power-of-two; overwrite on collision (lossy OK for forensics).
constexpr size_t kEnumSlotCap = 1u << 18; // 262144
constexpr size_t kEnumSlotMask = kEnumSlotCap - 1;

struct EnumSlotRec {
    std::atomic<uintptr_t> slot{0}; // 0 = empty
    std::atomic<uintptr_t> valueThen{0};
    std::atomic<size_t> gcIndex{0};
};

EnumSlotRec g_enumSlots[kEnumSlotCap];
std::atomic<size_t> g_enumNoteN{0};
std::atomic<size_t> g_enumCollideN{0};
std::atomic<size_t> g_enumEpochGc{0};

size_t SlotHash(uintptr_t s)
{
    // splitmix-ish mix for pointer addresses
    s ^= s >> 33;
    s *= 0xff51afd7ed558ccdULL;
    s ^= s >> 33;
    return static_cast<size_t>(s);
}

void MaybeRotateEpoch(size_t gc)
{
    size_t prev = g_enumEpochGc.load(std::memory_order_relaxed);
    if (prev == gc) {
        return;
    }
    if (g_enumEpochGc.compare_exchange_strong(prev, gc, std::memory_order_acq_rel)) {
        // Soft clear: only bump epoch; stale records filtered by gcIndex at lookup.
        g_enumNoteN.store(0, std::memory_order_relaxed);
        g_enumCollideN.store(0, std::memory_order_relaxed);
    }
}

void NoteEnumSlotImpl(void* slot, void* valueThen)
{
    if (slot == nullptr) {
        return;
    }
    const size_t gc = g_gcCount;
    MaybeRotateEpoch(gc);
    const uintptr_t s = reinterpret_cast<uintptr_t>(slot);
    const uintptr_t v = reinterpret_cast<uintptr_t>(valueThen);
    size_t idx = SlotHash(s) & kEnumSlotMask;
    for (size_t probe = 0; probe < 8; ++probe) {
        size_t i = (idx + probe) & kEnumSlotMask;
        uintptr_t cur = g_enumSlots[i].slot.load(std::memory_order_relaxed);
        if (cur == 0 || cur == s) {
            g_enumSlots[i].slot.store(s, std::memory_order_relaxed);
            g_enumSlots[i].valueThen.store(v, std::memory_order_relaxed);
            g_enumSlots[i].gcIndex.store(gc, std::memory_order_relaxed);
            g_enumNoteN.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    // force overwrite primary slot on full probe
    g_enumSlots[idx].slot.store(s, std::memory_order_relaxed);
    g_enumSlots[idx].valueThen.store(v, std::memory_order_relaxed);
    g_enumSlots[idx].gcIndex.store(gc, std::memory_order_relaxed);
    g_enumCollideN.fetch_add(1, std::memory_order_relaxed);
    g_enumNoteN.fetch_add(1, std::memory_order_relaxed);
}

struct SlotLookup {
    int found = 0;
    void* valueThen = nullptr;
    size_t gcIndex = 0;
};

SlotLookup LookupEnumSlot(void* slot)
{
    SlotLookup out;
    if (slot == nullptr) {
        return out;
    }
    const uintptr_t s = reinterpret_cast<uintptr_t>(slot);
    const size_t gc = g_gcCount;
    size_t idx = SlotHash(s) & kEnumSlotMask;
    for (size_t probe = 0; probe < 8; ++probe) {
        size_t i = (idx + probe) & kEnumSlotMask;
        if (g_enumSlots[i].slot.load(std::memory_order_relaxed) != s) {
            continue;
        }
        size_t recGc = g_enumSlots[i].gcIndex.load(std::memory_order_relaxed);
        // accept current or previous GC (F3 may fire after flip; enum recorded at g_gcCount)
        if (recGc != gc && recGc + 1 != gc) {
            continue;
        }
        out.found = 1;
        out.valueThen = reinterpret_cast<void*>(g_enumSlots[i].valueThen.load(std::memory_order_relaxed));
        out.gcIndex = recGc;
        return out;
    }
    return out;
}

// Secondary: any recorded enum value equal to holder (value-only, not identity).
int ValueSeenInEnum(void* holder, void** sampleSlotOut, void** sampleValueOut)
{
    if (holder == nullptr) {
        return 0;
    }
    const uintptr_t h = reinterpret_cast<uintptr_t>(holder);
    const size_t gc = g_gcCount;
    for (size_t i = 0; i < kEnumSlotCap; ++i) {
        uintptr_t s = g_enumSlots[i].slot.load(std::memory_order_relaxed);
        if (s == 0) {
            continue;
        }
        size_t recGc = g_enumSlots[i].gcIndex.load(std::memory_order_relaxed);
        if (recGc != gc && recGc + 1 != gc) {
            continue;
        }
        uintptr_t v = g_enumSlots[i].valueThen.load(std::memory_order_relaxed);
        if (v == h) {
            if (sampleSlotOut != nullptr) {
                *sampleSlotOut = reinterpret_cast<void*>(s);
            }
            if (sampleValueOut != nullptr) {
                *sampleValueOut = reinterpret_cast<void*>(v);
            }
            return 1;
        }
        // tagged low-48
        RefField<> rf(static_cast<MAddress>(v));
        if (rf.IsTagged() && reinterpret_cast<uintptr_t>(rf.GetTargetObject()) == h) {
            if (sampleSlotOut != nullptr) {
                *sampleSlotOut = reinterpret_cast<void*>(s);
            }
            if (sampleValueOut != nullptr) {
                *sampleValueOut = reinterpret_cast<void*>(v);
            }
            return 1;
        }
    }
    return 0;
}

struct Hit {
    int found = 0;
    void* sampleSlot = nullptr;
    void* sampleValue = nullptr;
    size_t visitN = 0;
};

bool ObjMatches(BaseObject* o, BaseObject* holder)
{
    if (o == nullptr || holder == nullptr) {
        return false;
    }
    if (o == holder) {
        return true;
    }
    // tagged root slot may carry bit48; GetTargetObject strips tag
    return false;
}

bool WordMatchesHolder(void* w, BaseObject* holder)
{
    if (w == nullptr || holder == nullptr) {
        return false;
    }
    if (w == holder) {
        return true;
    }
    // tagged pointer: low-48 address field
    RefField<> rf(reinterpret_cast<MAddress>(w));
    if (rf.IsTagged() && rf.GetTargetObject() == holder) {
        return true;
    }
    // untagged raw equal already handled; also try GetTargetObject on non-tagged heap addr
    if (Heap::IsHeapAddress(w)) {
        BaseObject* o = reinterpret_cast<BaseObject*>(w);
        return o == holder;
    }
    BaseObject* tgt = rf.GetTargetObject();
    return tgt == holder;
}

void VisitObjectRef(Hit& h, ObjectRef& root, BaseObject* holder)
{
    ++h.visitN;
    BaseObject* o = root.object;
    if (ObjMatches(o, holder) || (o != nullptr && WordMatchesHolder(o, holder))) {
        h.found = 1;
        if (h.sampleSlot == nullptr) {
            h.sampleSlot = &root;
            h.sampleValue = o;
        }
    }
}

void VisitRefField(Hit& h, RefField<>& rf, BaseObject* holder)
{
    ++h.visitN;
    BaseObject* tgt = rf.GetTargetObject();
    if (tgt == holder) {
        h.found = 1;
        if (h.sampleSlot == nullptr) {
            h.sampleSlot = &rf;
            h.sampleValue = tgt;
        }
    }
}

// Mode A family 1: precise mutator roots (stackmap + exception)
void ScanAStack(Hit& h, BaseObject* holder)
{
    MutatorManager::Instance().VisitAllMutators([&h, holder](Mutator& mut) {
        mut.VisitMutatorRoots([&h, holder](ObjectRef& root) { VisitObjectRef(h, root, holder); });
    });
}

// Mode A family 2: static
void ScanAStatic(Hit& h, BaseObject* holder)
{
    Heap::GetHeap().VisitStaticRoots([&h, holder](RefField<>& rf) { VisitRefField(h, rf, holder); });
}

// Mode A family 3: concurrency model
void ScanAConc(Hit& h, BaseObject* holder)
{
    RootVisitor v = [&h, holder](ObjectRef& root) { VisitObjectRef(h, root, holder); };
    Runtime::Current().GetConcurrencyModel().VisitGCRoots(&v);
}

// Mode A family 4+5: finalizer GC roots + finalizer list
void ScanAFinalizer(Hit& hRoots, Hit& hQueue, BaseObject* holder, FinalizerProcessor& fp)
{
    fp.VisitGCRoots([&hRoots, holder](ObjectRef& root) { VisitObjectRef(hRoots, root, holder); });
    fp.VisitFinalizers([&hQueue, holder](ObjectRef& root) { VisitObjectRef(hQueue, root, holder); });
}

// Mode A family 6: export
void ScanAExport(Hit& h, BaseObject* holder)
{
    Heap::GetHeap().VisitAllExportRoots([&h, holder](ObjectRef& root) { VisitObjectRef(h, root, holder); });
}

// Mode B: conservative stack word scan
void ScanBStackCons(Hit& h, BaseObject* holder)
{
    MutatorManager::Instance().VisitAllMutators([&h, holder](Mutator& mut) {
        uintptr_t top = mut.GetStackTopAddr();
        uintptr_t sz = mut.GetStackSize();
        if (top == 0 || sz == 0 || sz > (1ull << 28)) {
            return;
        }
        const size_t words = sz / sizeof(void*);
        auto* base = reinterpret_cast<void**>(top);
        for (size_t i = 0; i < words; ++i) {
            ++h.visitN;
            void* w = base[i];
            if (!WordMatchesHolder(w, holder)) {
                continue;
            }
            h.found = 1;
            if (h.sampleSlot == nullptr) {
                h.sampleSlot = &base[i];
                h.sampleValue = w;
            }
            return;
        }
    });
}

const char* ReasonNameLocal(StackMapInvalidReason r)
{
    switch (r) {
        case StackMapInvalidReason::NONE:
            return "none";
        case StackMapInvalidReason::ZERO_ENTRIES:
            return "ZERO_ENTRIES";
        case StackMapInvalidReason::PC_MISS:
            return "PC_MISS";
        case StackMapInvalidReason::ZERO_ROOT_INDICES:
            return "ZERO_ROOT_INDICES";
    }
    return "unknown";
}

// Dump precise stackmap state for every MANAGED frame on every mutator (same path as VisitMutatorRoots).
// Also attribute consSlot to FA range. Logs [GCV2][B3ROOT][FRAME]/FDUMP]/FSUM].
void AttributeConsSlotToFrame(void* consSlot, BaseObject* holder)
{
    if (consSlot == nullptr || holder == nullptr) {
        return;
    }
    const uintptr_t slotAddr = reinterpret_cast<uintptr_t>(consSlot);
    size_t totalManaged = 0;
    size_t totalValid = 0;
    size_t totalMiss = 0;
    size_t totalZero = 0;
    size_t totalOther = 0;
    int foundOwner = 0;

    MutatorManager::Instance().VisitAllMutators(
        [slotAddr, holder, consSlot, &totalManaged, &totalValid, &totalMiss, &totalZero, &totalOther,
         &foundOwner](Mutator& mut) {
            if (!mut.IsManagedContext()) {
                return;
            }
            uintptr_t top = mut.GetStackTopAddr();
            GCStackInfo gcStackInfo(&mut.GetUnwindContext());
            gcStackInfo.FillInStackTrace();
            auto& frames = gcStackInfo.GetStack();
            size_t managedN = 0;
            size_t validN = 0;
            size_t missN = 0;
            size_t zeroN = 0;
            for (size_t i = 0; i < frames.size() && i < 48; ++i) {
                FrameInfo& fr = frames[i];
                if (fr.GetFrameType() != FrameType::MANAGED) {
                    continue;
                }
                ++managedN;
                ++totalManaged;
                uintptr_t fa = reinterpret_cast<uintptr_t>(fr.mFrame.GetFA());
                uintptr_t frameIP = reinterpret_cast<uintptr_t>(fr.mFrame.GetIP());
                if (fr.GetStartProc() == nullptr) {
                    fr.ResolveProcInfo();
                }
                uintptr_t startIP = reinterpret_cast<uintptr_t>(fr.GetStartProc());
                CString fname;
                if (startIP != 0) {
                    FuncDescRef desc = MFuncDesc::GetFuncDesc(startIP);
                    if (desc != nullptr) {
                        fname = desc->GetFuncName();
                    }
                }
                if (fname.IsEmpty()) {
                    fname = fr.GetFuncName();
                }
                const char* sym = fname.IsEmpty() ? "?" : fname.Str();
                const char* mapState = "NO_START";
                int inPrecise = 0;
                int preciseSlotN = 0;
                U32 pcOff = 0;
                if (startIP != 0 && frameIP != 0) {
                    pcOff = (frameIP >= startIP) ? static_cast<U32>(frameIP - startIP) : 0;
                    StackMapBuilder builder(startIP, frameIP, fa);
                    RootMap rootMap = builder.Build<RootMap>();
                    if (rootMap.IsValid()) {
                        mapState = "VALID";
                        ++validN;
                        ++totalValid;
                        rootMap.VisitSlotRoots(
                            [](ObjectRef&) {},
                            [slotAddr, &inPrecise, &preciseSlotN, fa](SlotBias bias, BaseObject*) {
                                ++preciseSlotN;
                                uintptr_t s = static_cast<uintptr_t>(static_cast<intptr_t>(fa) + bias);
                                if (s == slotAddr) {
                                    inPrecise = 1;
                                }
                            });
                    } else {
                        StackMapInvalidReason r = builder.GetInvalidReason();
                        mapState = ReasonNameLocal(r);
                        if (r == StackMapInvalidReason::ZERO_ENTRIES) {
                            ++zeroN;
                            ++totalZero;
                        } else if (r == StackMapInvalidReason::PC_MISS) {
                            ++missN;
                            ++totalMiss;
                        } else {
                            ++totalOther;
                        }
                    }
                }
                // slot ownership: between this FA and previous (deeper) frame FA; frames[0]=top
                uintptr_t lo = top;
                if (i + 1 < frames.size()) {
                    lo = reinterpret_cast<uintptr_t>(frames[i + 1].mFrame.GetFA());
                }
                // also accept slot within 8KB below FA (locals)
                int owns = 0;
                if (slotAddr < fa && slotAddr >= lo) {
                    owns = 1;
                } else if (slotAddr < fa && (fa - slotAddr) <= 8192) {
                    owns = 1;
                }
                int isEntryShell = (std::strcmp(sym, "user.main") == 0 || std::strcmp(sym, "cj_entry$") == 0) ? 1 : 0;
                intptr_t offFromFA = static_cast<intptr_t>(fa) - static_cast<intptr_t>(slotAddr);
                if (owns && !foundOwner) {
                    foundOwner = 1;
                    VLOG(REPORT,
                         "[GCV2][B3ROOT][FRAME] consSlot=%p holder=%p frameIdx=%zu symbol=%s start_ip=%p frame_ip=%p "
                         "pc_off=%u fa=%p offFromFA=%zd mapState=%s inPreciseSlot=%d preciseSlotN=%d entryShell=%d",
                         consSlot, holder, i, sym, reinterpret_cast<void*>(startIP), reinterpret_cast<void*>(frameIP),
                         pcOff, reinterpret_cast<void*>(fa), offFromFA, mapState, inPrecise, preciseSlotN,
                         isEntryShell);
                }
                // dump every managed frame (cap) so we see miss concentration
                VLOG(REPORT,
                     "[GCV2][B3ROOT][FDUMP] mut=%p frameIdx=%zu fType=MANAGED symbol=%s pc_off=%u fa=%p "
                     "mapState=%s preciseSlotN=%d entryShell=%d owns=%d offFromFA=%zd",
                     &mut, i, sym, pcOff, reinterpret_cast<void*>(fa), mapState, preciseSlotN, isEntryShell, owns,
                     offFromFA);
            }
            VLOG(REPORT,
                 "[GCV2][B3ROOT][MUTSUM] mut=%p frames=%zu managedN=%zu validN=%zu missN=%zu zeroN=%zu "
                 "managedCtx=1 top=%p",
                 &mut, frames.size(), managedN, validN, missN, zeroN, reinterpret_cast<void*>(top));
        });

    VLOG(REPORT,
         "[GCV2][B3ROOT][FSUM] consSlot=%p holder=%p managedN=%zu validN=%zu missN=%zu zeroN=%zu otherN=%zu "
         "foundOwner=%d",
         consSlot, holder, totalManaged, totalValid, totalMiss, totalZero, totalOther, foundOwner);
}

// Mode B static: same VisitStaticRoots (no wider static map API); count separately for A/B delta
void ScanBStatic(Hit& h, BaseObject* holder) { ScanAStatic(h, holder); }

// Mode B TLS proxy: concurrency model roots (thread-local / scheduler)
void ScanBTls(Hit& h, BaseObject* holder) { ScanAConc(h, holder); }

const char* FirstAFamily(const Hit& aStack, const Hit& aStatic, const Hit& aConc, const Hit& aFinR,
                         const Hit& aFinQ, const Hit& aExport)
{
    if (aStack.found) {
        return "mutator_stack";
    }
    if (aStatic.found) {
        return "static";
    }
    if (aConc.found) {
        return "concurrency";
    }
    if (aFinR.found) {
        return "finalizer_roots";
    }
    if (aFinQ.found) {
        return "finalizer_queue";
    }
    if (aExport.found) {
        return "export";
    }
    return "none";
}

const char* FirstBFamily(const Hit& bStack, const Hit& bStatic, const Hit& bTls)
{
    if (bStack.found) {
        return "stack_cons";
    }
    if (bStatic.found) {
        return "static";
    }
    if (bTls.found) {
        return "tls_conc";
    }
    return "none";
}

void* FirstASlot(const Hit& aStack, const Hit& aStatic, const Hit& aConc, const Hit& aFinR, const Hit& aFinQ,
                 const Hit& aExport)
{
    if (aStack.found) {
        return aStack.sampleSlot;
    }
    if (aStatic.found) {
        return aStatic.sampleSlot;
    }
    if (aConc.found) {
        return aConc.sampleSlot;
    }
    if (aFinR.found) {
        return aFinR.sampleSlot;
    }
    if (aFinQ.found) {
        return aFinQ.sampleSlot;
    }
    if (aExport.found) {
        return aExport.sampleSlot;
    }
    return nullptr;
}

void* FirstBSlot(const Hit& bStack, const Hit& bStatic, const Hit& bTls)
{
    if (bStack.found) {
        return bStack.sampleSlot;
    }
    if (bStatic.found) {
        return bStatic.sampleSlot;
    }
    if (bTls.found) {
        return bTls.sampleSlot;
    }
    return nullptr;
}

// b3origin T0/T1: reverse in-edges to holder (heap fields that currently hold holder).
// Answers: last materialisation source = HEAP_FIELD vs no heap field (REG_SPILL/ALLOC/OTHER).
// Also remset Contains on each sample edge for B3O_REMSET_GAP vs IN_REMSET.
struct InEdgeSample {
    size_t edgeN = 0;
    size_t oldReferrerN = 0;
    size_t youngReferrerN = 0;
    size_t markedReferrerN = 0;
    size_t remsetHitN = 0;
    size_t remsetMissN = 0;
    BaseObject* sampleReferrer = nullptr;
    void* sampleSlot = nullptr;
    int sampleYoung = -1;
    int sampleMarked = -1;
    int sampleRemset = -1;
    size_t sampleSlotOff = 0;
};

void ScanHeapInEdges(InEdgeSample& out, BaseObject* holderObj)
{
    if (holderObj == nullptr) {
        return;
    }
    RememberedSet& rs = Heap::GetHeap().GetRememberedSet();
    Heap::GetHeap().ForEachObj(
        [holderObj, &out, &rs](BaseObject* cand) {
            if (cand == nullptr || cand == holderObj) {
                return;
            }
            if (!cand->IsValidObject() || !cand->HasRefField()) {
                return;
            }
            cand->ForEachRefField([holderObj, cand, &out, &rs](RefField<>& rf) {
                BaseObject* tgt = rf.GetTargetObject();
                if (tgt != holderObj) {
                    return;
                }
                ++out.edgeN;
                int young = -1;
                RegionInfo* reg = RegionInfo::TryGetRegionInfoAt(reinterpret_cast<uintptr_t>(cand));
                if (reg != nullptr) {
                    young = reg->IsYoungRegion() ? 1 : 0;
                    if (young == 1) {
                        ++out.youngReferrerN;
                    } else {
                        ++out.oldReferrerN;
                    }
                }
                int marked = RegionSpace::IsMarkedObject(cand) ? 1 : 0;
                if (marked == 1) {
                    ++out.markedReferrerN;
                }
                MAddress slot = reinterpret_cast<MAddress>(&rf);
                int inRemset = rs.Contains(slot) ? 1 : 0;
                if (inRemset == 1) {
                    ++out.remsetHitN;
                } else {
                    ++out.remsetMissN;
                }
                if (out.sampleReferrer == nullptr) {
                    out.sampleReferrer = cand;
                    out.sampleSlot = reinterpret_cast<void*>(slot);
                    out.sampleYoung = young;
                    out.sampleMarked = marked;
                    out.sampleRemset = inRemset;
                    out.sampleSlotOff = static_cast<size_t>(slot - reinterpret_cast<MAddress>(cand));
                }
            });
        },
        false);
}

} // namespace

bool B3Root::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_B3ROOT");
    return on;
}

void B3Root::NoteEnumSlot(void* slot, void* valueThen)
{
    if (!Enabled()) {
        return;
    }
    NoteEnumSlotImpl(slot, valueThen);
}

void B3Root::ClassifyHolder(void* holder, int holderValid, int holderMarked, void* fieldAddr, int loadFromHeapField,
                            void* /*collectorOpaque*/)
{
    if (!Enabled() || holder == nullptr) {
        return;
    }
    auto* holderObj = reinterpret_cast<BaseObject*>(holder);

    // Heartbeat first so SEGV during later scans still leaves a sample trail.
    VLOG(REPORT, "[GCV2][B3ROOT][BEGIN] holder=%p holderValid=%d holderMarked=%d field=%p loadFromHeapField=%d",
         holder, holderValid, holderMarked, fieldAddr, loadFromHeapField);

    Hit aStack, aStatic, aConc, aFinR, aFinQ, aExport;
    Hit bStack, bStatic, bTls;

    // Wide stack first (most discriminative for ENUM_MISSES_STACK); log partial early.
    ScanBStackCons(bStack, holderObj);
    VLOG(REPORT, "[GCV2][B3ROOT][STEP] holder=%p step=B_stack found=%d N=%zu slot=%p", holder, bStack.found,
         bStack.visitN, bStack.sampleSlot);

    // ⭐ T0 identity: was B_slot / holder seen during concurrent enum?
    //   ENUMERATED_AT_ENUM=YES + value_then!=value_now ⇒ B3_CASCADE
    //   ENUMERATED_AT_ENUM=NO (slot absent) + B_stack hit ⇒ B3_TRUE_MISS
    void* valueNowAtB = nullptr;
    void* valueNowUntagged = nullptr;
    if (bStack.found && bStack.sampleSlot != nullptr) {
        valueNowAtB = *reinterpret_cast<void**>(bStack.sampleSlot);
        // NoteEnumSlot stores GetTargetObject (untagged); normalize value_now the same way
        RefField<> nowRf(reinterpret_cast<MAddress>(valueNowAtB));
        valueNowUntagged = nowRf.IsTagged() ? static_cast<void*>(nowRf.GetTargetObject()) : valueNowAtB;
    }
    SlotLookup slotLk = LookupEnumSlot(bStack.sampleSlot);
    void* valueSeenSlot = nullptr;
    void* valueSeenVal = nullptr;
    int valueSeen = ValueSeenInEnum(holder, &valueSeenSlot, &valueSeenVal);
    int valueThenEqNow = 0;
    int valueThenEqHolder = 0;
    if (slotLk.found) {
        valueThenEqNow = (slotLk.valueThen == valueNowUntagged || slotLk.valueThen == valueNowAtB) ? 1 : 0;
        valueThenEqHolder =
            (slotLk.valueThen == holder ||
             (slotLk.valueThen != nullptr && WordMatchesHolder(slotLk.valueThen, holderObj)))
                ? 1
                : 0;
    }
    const char* t0 = "B3_T0_UNKNOWN";
    if (bStack.found && slotLk.found && !valueThenEqHolder && !valueThenEqNow) {
        t0 = "B3_CASCADE"; // slot was enumerated; value at enum ≠ holder now
    } else if (bStack.found && slotLk.found && valueThenEqHolder) {
        t0 = "B3_ENUM_SAW_HOLDER"; // precise enum saw this slot with holder value
    } else if (bStack.found && !slotLk.found && !valueSeen) {
        t0 = "B3_TRUE_MISS"; // cons slot never in enum ledger
    } else if (bStack.found && !slotLk.found && valueSeen) {
        t0 = "B3_VALUE_SEEN_OTHER_SLOT"; // holder value pushed from a different slot
    } else if (!bStack.found && valueSeen) {
        t0 = "B3_ENUM_VALUE_NO_CONS"; // enum saw value, cons miss (unexpected)
    }
    VLOG(REPORT,
         "[GCV2][B3ROOT][T0] holder=%p B_slot=%p value_now=%p ENUMERATED_AT_ENUM=%d value_then=%p "
         "value_then_eq_now=%d value_then_eq_holder=%d valueSeenOther=%d seenSlot=%p "
         "enumNoteN=%zu enumCollideN=%zu gcIndex=%zu t0=%s",
         holder, bStack.sampleSlot, valueNowAtB, slotLk.found, slotLk.valueThen, valueThenEqNow, valueThenEqHolder,
         valueSeen, valueSeenSlot, g_enumNoteN.load(std::memory_order_relaxed),
         g_enumCollideN.load(std::memory_order_relaxed), slotLk.gcIndex, t0);
    if (std::strcmp(t0, "B3_CASCADE") == 0) {
        VLOG(REPORT,
             "[GCV2][B3ROOT][CASCADE] holder=%p slot=%p value_then=%p value_now=%p "
             "reason=slot_enumerated_value_rewritten_after_enum",
             holder, bStack.sampleSlot, slotLk.valueThen, valueNowAtB);
    }

    ScanAStack(aStack, holderObj);
    VLOG(REPORT, "[GCV2][B3ROOT][STEP] holder=%p step=A_stack found=%d N=%zu slot=%p", holder, aStack.found,
         aStack.visitN, aStack.sampleSlot);

    // Early trichotomy on stack alone (covers majority of B-3 samples).
    if (aStack.found) {
        VLOG(REPORT,
             "[GCV2][B3ROOT] holder=%p holderValid=%d holderMarked=%d verdict=B3ROOT_A_FOUND_MARK_FAILED "
             "A_found=1 A_family=mutator_stack A_slot=%p B_found=%d B_family=%s B_slot=%p "
             "A_stack=1 A_static=-1 A_conc=-1 A_finR=-1 A_finQ=-1 A_export=-1 "
             "B_stack=%d B_static=-1 B_tls=-1 A_stackN=%zu B_stackN=%zu field=%p loadFromHeapField=%d "
             "t0=%s partial=1",
             holder, holderValid, holderMarked, aStack.sampleSlot, bStack.found,
             bStack.found ? "stack_cons" : "none", bStack.sampleSlot, bStack.found, aStack.visitN, bStack.visitN,
             fieldAddr, loadFromHeapField, t0);
        return;
    }
    if (bStack.found) {
        VLOG(REPORT,
             "[GCV2][B3ROOT] holder=%p holderValid=%d holderMarked=%d verdict=B3ROOT_ENUM_MISSES_STACK "
             "A_found=0 A_family=none A_slot=(nil) B_found=1 B_family=stack_cons B_slot=%p "
             "A_stack=0 A_static=-1 A_conc=-1 A_finR=-1 A_finQ=-1 A_export=-1 "
             "B_stack=1 B_static=-1 B_tls=-1 A_stackN=%zu B_stackN=%zu field=%p loadFromHeapField=%d "
             "t0=%s partial=1",
             holder, holderValid, holderMarked, bStack.sampleSlot, aStack.visitN, bStack.visitN, fieldAddr,
             loadFromHeapField, t0);
        VLOG(REPORT,
             "[GCV2][B3ROOT][ENUM_MISS_DETAIL] family=STACK reason=precise_VisitMutatorRoots_miss_wide_cons_hit "
             "consSlot=%p consValue=%p A_stackN=%zu B_stackN=%zu t0=%s",
             bStack.sampleSlot, bStack.sampleValue, aStack.visitN, bStack.visitN, t0);
        // b3origin T0/T1: reverse heap in-edges → source class + remset gap
        InEdgeSample inEdges;
        ScanHeapInEdges(inEdges, holderObj);
        const char* srcClass = "B3O_SOURCE_OTHER";
        if (inEdges.edgeN > 0) {
            srcClass = "B3O_SOURCE_HEAP_FIELD";
        } else if (bStack.found) {
            srcClass = "B3O_SOURCE_REG_SPILL_OR_STACK_ONLY";
        }
        const char* remClass = "B3O_REMSET_NA";
        if (inEdges.edgeN == 0) {
            remClass = "B3O_REMSET_NA_no_inedge";
        } else if (inEdges.oldReferrerN > 0 && inEdges.remsetMissN > 0 && inEdges.remsetHitN == 0) {
            remClass = "B3O_REMSET_GAP";
        } else if (inEdges.remsetHitN > 0) {
            remClass = "B3O_IN_REMSET";
        } else if (inEdges.youngReferrerN == inEdges.edgeN) {
            remClass = "B3O_REMSET_NA_all_young_referrers";
        } else {
            remClass = "B3O_REMSET_MISS_or_drained";
        }
        VLOG(REPORT,
             "[GCV2][B3O][ORIGIN] holder=%p B_slot=%p srcClass=%s remClass=%s "
             "inEdgeN=%zu oldRefN=%zu youngRefN=%zu markedRefN=%zu remsetHitN=%zu remsetMissN=%zu "
             "sampleReferrer=%p sampleSlot=%p sampleOff=%zu sampleYoung=%d sampleMarked=%d sampleRemset=%d "
             "field=%p t0=%s",
             holder, bStack.sampleSlot, srcClass, remClass, inEdges.edgeN, inEdges.oldReferrerN,
             inEdges.youngReferrerN, inEdges.markedReferrerN, inEdges.remsetHitN, inEdges.remsetMissN,
             inEdges.sampleReferrer, inEdges.sampleSlot, inEdges.sampleSlotOff, inEdges.sampleYoung,
             inEdges.sampleMarked, inEdges.sampleRemset, fieldAddr, t0);
        // T1: which frame holds the cons slot + stackmap state (before other family scans that may SEGV)
        AttributeConsSlotToFrame(bStack.sampleSlot, holderObj);
        // continue remaining families for completeness when possible
    }

    ScanAStatic(aStatic, holderObj);
    ScanAConc(aConc, holderObj);
    ScanAFinalizer(aFinR, aFinQ, holderObj, Heap::GetHeap().GetFinalizerProcessor());
    ScanAExport(aExport, holderObj);
    ScanBStatic(bStatic, holderObj);
    ScanBTls(bTls, holderObj);

    const int aAny = aStack.found | aStatic.found | aConc.found | aFinR.found | aFinQ.found | aExport.found;
    const int bAny = bStack.found | bStatic.found | bTls.found;

    const char* aFamily = FirstAFamily(aStack, aStatic, aConc, aFinR, aFinQ, aExport);
    const char* bFamily = FirstBFamily(bStack, bStatic, bTls);
    void* aSlot = FirstASlot(aStack, aStatic, aConc, aFinR, aFinQ, aExport);
    void* bSlot = FirstBSlot(bStack, bStatic, bTls);

    const char* verdict = "B3ROOT_UNKNOWN";
    if (aAny) {
        verdict = "B3ROOT_A_FOUND_MARK_FAILED";
    } else if (bAny) {
        if (bStack.found) {
            verdict = "B3ROOT_ENUM_MISSES_STACK";
        } else if (bStatic.found) {
            verdict = "B3ROOT_ENUM_MISSES_STATIC";
        } else {
            verdict = "B3ROOT_ENUM_MISSES_TLS";
        }
    } else {
        verdict = "B3ROOT_NO_HOLDER";
    }

    VLOG(REPORT,
         "[GCV2][B3ROOT] holder=%p holderValid=%d holderMarked=%d verdict=%s "
         "A_found=%d A_family=%s A_slot=%p "
         "A_stack=%d A_static=%d A_conc=%d A_finR=%d A_finQ=%d A_export=%d "
         "A_stackN=%zu A_staticN=%zu A_concN=%zu A_finRN=%zu A_finQN=%zu A_exportN=%zu "
         "B_found=%d B_family=%s B_slot=%p "
         "B_stack=%d B_static=%d B_tls=%d "
          "B_stackN=%zu B_staticN=%zu B_tlsN=%zu "
          "field=%p loadFromHeapField=%d t0=%s partial=0",
          holder, holderValid, holderMarked, verdict, aAny, aFamily, aSlot, aStack.found, aStatic.found, aConc.found,
          aFinR.found, aFinQ.found, aExport.found, aStack.visitN, aStatic.visitN, aConc.visitN, aFinR.visitN,
          aFinQ.visitN, aExport.visitN, bAny, bFamily, bSlot, bStack.found, bStatic.found, bTls.found, bStack.visitN,
          bStatic.visitN, bTls.visitN, fieldAddr, loadFromHeapField, t0);

    if (!aAny && bStack.found) {
        VLOG(REPORT,
             "[GCV2][B3ROOT][ENUM_MISS_DETAIL] family=STACK reason=precise_VisitMutatorRoots_miss_wide_cons_hit "
             "consSlot=%p consValue=%p A_stackN=%zu B_stackN=%zu",
             bStack.sampleSlot, bStack.sampleValue, aStack.visitN, bStack.visitN);
    }
    if (!aAny && !bAny) {
        VLOG(REPORT,
             "[GCV2][B3ROOT][NO_HOLDER_DETAIL] mutator_read field=%p loadFromHeapField=%d "
             "hint=%s",
             fieldAddr, loadFromHeapField,
             loadFromHeapField ? "heap_field_memory_load" : "root_or_null_holder_field");
    }
}

} // namespace MapleRuntime
