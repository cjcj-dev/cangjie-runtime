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
#include "Common/Runtime.h"
#include "Common/StackType.h"
#include "Concurrency/Concurrency.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/MFuncdesc.inline.h"
#include "ObjectModel/RefField.inline.h"
#include "StackMap/CompressedStackMap.h"
#include "StackMap/StackMap.h"

namespace MapleRuntime {
namespace {

bool EnvIsOne(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && std::strcmp(v, "1") == 0;
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

// Attribute B_slot via FA chain walk (no Mutator uwContext — F3 abort often has empty/unreliable context).
// x86_64 Cangjie frame: [FA]=callerFA, [FA+8]=return IP; startPC recovered via GetFuncStartPCFromFrameAddress.
// Logs [GCV2][B3ROOT][FRAME].
void AttributeConsSlotToFrame(void* consSlot, BaseObject* holder)
{
    if (consSlot == nullptr || holder == nullptr) {
        return;
    }
    const uintptr_t slotAddr = reinterpret_cast<uintptr_t>(consSlot);
    MutatorManager::Instance().VisitAllMutators([slotAddr, holder, consSlot](Mutator& mut) {
        uintptr_t top = mut.GetStackTopAddr();
        uintptr_t sz = mut.GetStackSize();
        if (top == 0 || sz == 0 || sz > (1ull << 28)) {
            return;
        }
        if (slotAddr < top || slotAddr >= top + sz) {
            return;
        }
        // Seed FA: nearest saved FA word at/above slot that looks like a frame chain into this stack.
        FrameAddress* fa = nullptr;
        {
            // scan upward from slot (toward higher addr) for a plausible FA pointer
            uintptr_t scan = (slotAddr + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
            uintptr_t hi = top + sz;
            for (size_t n = 0; n < 4096 && scan + sizeof(void*) <= hi; ++n, scan += sizeof(void*)) {
                auto* cand = *reinterpret_cast<FrameAddress**>(scan);
                uintptr_t c = reinterpret_cast<uintptr_t>(cand);
                if (c > scan && c < hi && ((c & 0x7) == 0)) {
                    // cand looks like FA further up the stack; treat scan itself as FA if [scan] is caller
                    // Prefer: scan is the FA of the frame that owns slot (slot < FA).
                    if (scan > slotAddr) {
                        fa = reinterpret_cast<FrameAddress*>(scan);
                        break;
                    }
                }
            }
        }
        // Fallback: walk from stack top region looking for FA chain containing slot
        if (fa == nullptr) {
            // use mutator recorded FA if in range
            FrameAddress* mutFa = mut.GetUnwindContext().frameInfo.mFrame.GetFA();
            uintptr_t mfa = reinterpret_cast<uintptr_t>(mutFa);
            if (mutFa != nullptr && mfa > slotAddr && mfa < top + sz) {
                fa = mutFa;
            }
        }
        if (fa == nullptr) {
            VLOG(REPORT, "[GCV2][B3ROOT][FRAME] consSlot=%p holder=%p NO_FA_SEED top=%p sz=%zu", consSlot, holder,
                 reinterpret_cast<void*>(top), sz);
            return;
        }
        // Walk FA chain upward (caller direction) until FA > slot; the first FA > slot owns the locals below it.
        size_t frameIdx = 0;
        FrameAddress* cur = fa;
        FrameAddress* owner = nullptr;
        for (; cur != nullptr && frameIdx < 256; ++frameIdx) {
            uintptr_t cfa = reinterpret_cast<uintptr_t>(cur);
            if (cfa <= top || cfa >= top + sz) {
                break;
            }
            if (cfa > slotAddr) {
                owner = cur;
                break;
            }
            cur = cur->callerFrameAddress;
        }
        if (owner == nullptr) {
            VLOG(REPORT, "[GCV2][B3ROOT][FRAME] consSlot=%p holder=%p NO_OWNER_FA seed=%p walked=%zu", consSlot, holder,
                 fa, frameIdx);
            return;
        }
        uintptr_t ownerFA = reinterpret_cast<uintptr_t>(owner);
        // return IP sits at FA+sizeof(void*) on x86_64 System V
        uintptr_t retIP = *reinterpret_cast<uintptr_t*>(ownerFA + sizeof(void*));
        uint32_t* startPCPtr = FrameInfo::GetFuncStartPCFromFrameAddress(owner);
        uintptr_t startIP = reinterpret_cast<uintptr_t>(startPCPtr);
        // frameIP for stackmap is the IP at safepoint; retIP is the call-return site (same as unwind IP)
        uintptr_t frameIP = retIP;
        const char* sym = "?";
        CString fname;
        int hasDesc = 0;
        U32 codeSize = 0;
        if (startIP != 0) {
            FuncDescRef desc = MFuncDesc::GetFuncDesc(startIP);
            if (desc != nullptr) {
                hasDesc = 1;
                fname = desc->GetFuncName();
                if (!fname.IsEmpty()) {
                    sym = fname.Str();
                }
                codeSize = desc->GetCodeSize();
            }
        }
        // stackmap at this PC
        const char* mapState = "NO_START";
        int inPrecise = 0;
        int preciseSlotN = 0;
        U32 pcOff = 0;
        if (startIP != 0 && frameIP != 0) {
            pcOff = (frameIP >= startIP) ? static_cast<U32>(frameIP - startIP) : 0;
            StackMapBuilder builder(startIP, frameIP, ownerFA);
            RootMap rootMap = builder.Build<RootMap>();
            if (rootMap.IsValid()) {
                mapState = "VALID";
                rootMap.VisitSlotRoots(
                    [](ObjectRef&) {},
                    [slotAddr, &inPrecise, &preciseSlotN, ownerFA](SlotBias bias, BaseObject*) {
                        ++preciseSlotN;
                        uintptr_t s = static_cast<uintptr_t>(static_cast<intptr_t>(ownerFA) + bias);
                        if (s == slotAddr) {
                            inPrecise = 1;
                        }
                    });
            } else {
                mapState = ReasonNameLocal(builder.GetInvalidReason());
            }
        }
        int isEntryShell = (std::strcmp(sym, "user.main") == 0 || std::strcmp(sym, "cj_entry$") == 0) ? 1 : 0;
        intptr_t offFromFA = static_cast<intptr_t>(ownerFA) - static_cast<intptr_t>(slotAddr);
        VLOG(REPORT,
             "[GCV2][B3ROOT][FRAME] consSlot=%p holder=%p frameIdx=%zu symbol=%s start_ip=%p frame_ip=%p "
             "pc_off=%u fa=%p offFromFA=%zd mapState=%s inPreciseSlot=%d preciseSlotN=%d entryShell=%d "
             "hasDesc=%d codeSize=%u",
             consSlot, holder, frameIdx, sym, reinterpret_cast<void*>(startIP), reinterpret_cast<void*>(frameIP), pcOff,
             reinterpret_cast<void*>(ownerFA), offFromFA, mapState, inPrecise, preciseSlotN, isEntryShell, hasDesc,
             codeSize);
    });
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

} // namespace

bool B3Root::Enabled()
{
    static const bool on = EnvIsOne("MRT_GCV2_B3ROOT");
    return on;
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

    ScanAStack(aStack, holderObj);
    VLOG(REPORT, "[GCV2][B3ROOT][STEP] holder=%p step=A_stack found=%d N=%zu slot=%p", holder, aStack.found,
         aStack.visitN, aStack.sampleSlot);

    // Early trichotomy on stack alone (covers majority of B-3 samples).
    if (aStack.found) {
        VLOG(REPORT,
             "[GCV2][B3ROOT] holder=%p holderValid=%d holderMarked=%d verdict=B3ROOT_A_FOUND_MARK_FAILED "
             "A_found=1 A_family=mutator_stack A_slot=%p B_found=%d B_family=%s B_slot=%p "
             "A_stack=1 A_static=-1 A_conc=-1 A_finR=-1 A_finQ=-1 A_export=-1 "
             "B_stack=%d B_static=-1 B_tls=-1 A_stackN=%zu B_stackN=%zu field=%p loadFromHeapField=%d partial=1",
             holder, holderValid, holderMarked, aStack.sampleSlot, bStack.found,
             bStack.found ? "stack_cons" : "none", bStack.sampleSlot, bStack.found, aStack.visitN, bStack.visitN,
             fieldAddr, loadFromHeapField);
        return;
    }
    if (bStack.found) {
        VLOG(REPORT,
             "[GCV2][B3ROOT] holder=%p holderValid=%d holderMarked=%d verdict=B3ROOT_ENUM_MISSES_STACK "
             "A_found=0 A_family=none A_slot=(nil) B_found=1 B_family=stack_cons B_slot=%p "
             "A_stack=0 A_static=-1 A_conc=-1 A_finR=-1 A_finQ=-1 A_export=-1 "
             "B_stack=1 B_static=-1 B_tls=-1 A_stackN=%zu B_stackN=%zu field=%p loadFromHeapField=%d partial=1",
             holder, holderValid, holderMarked, bStack.sampleSlot, aStack.visitN, bStack.visitN, fieldAddr,
             loadFromHeapField);
        VLOG(REPORT,
             "[GCV2][B3ROOT][ENUM_MISS_DETAIL] family=STACK reason=precise_VisitMutatorRoots_miss_wide_cons_hit "
             "consSlot=%p consValue=%p A_stackN=%zu B_stackN=%zu",
             bStack.sampleSlot, bStack.sampleValue, aStack.visitN, bStack.visitN);
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
         "field=%p loadFromHeapField=%d partial=0",
         holder, holderValid, holderMarked, verdict, aAny, aFamily, aSlot, aStack.found, aStatic.found, aConc.found,
         aFinR.found, aFinQ.found, aExport.found, aStack.visitN, aStatic.visitN, aConc.visitN, aFinR.visitN,
         aFinQ.visitN, aExport.visitN, bAny, bFamily, bSlot, bStack.found, bStatic.found, bTls.found, bStack.visitN,
         bStatic.visitN, bTls.visitN, fieldAddr, loadFromHeapField);

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
