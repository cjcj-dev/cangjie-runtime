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
#include "Concurrency/Concurrency.h"
#include "Heap/Heap.h"
#include "Mutator/Mutator.h"
#include "Mutator/MutatorManager.h"
#include "ObjectModel/RefField.inline.h"

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

    Hit aStack, aStatic, aConc, aFinR, aFinQ, aExport;
    Hit bStack, bStatic, bTls;

    ScanAStack(aStack, holderObj);
    ScanAStatic(aStatic, holderObj);
    ScanAConc(aConc, holderObj);
    ScanAFinalizer(aFinR, aFinQ, holderObj, Heap::GetHeap().GetFinalizerProcessor());
    ScanAExport(aExport, holderObj);

    ScanBStackCons(bStack, holderObj);
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
        // ENUM missed a root that wide scan found
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

    // If A missed stack but B hit stack → classic stackmap miss (even if A found elsewhere)
    // Prefer the task's primary trichotomy: A-found vs A-miss-B-hit vs both-miss.
    // When A found anywhere, keep A_FOUND (mark step failed after enum).
    // When A none and B hit: ENUM_MISSES.
    // When both none: NO_HOLDER.

    VLOG(REPORT,
         "[GCV2][B3ROOT] holder=%p holderValid=%d holderMarked=%d verdict=%s "
         "A_found=%d A_family=%s A_slot=%p "
         "A_stack=%d A_static=%d A_conc=%d A_finR=%d A_finQ=%d A_export=%d "
         "A_stackN=%zu A_staticN=%zu A_concN=%zu A_finRN=%zu A_finQN=%zu A_exportN=%zu "
         "B_found=%d B_family=%s B_slot=%p "
         "B_stack=%d B_static=%d B_tls=%d "
         "B_stackN=%zu B_staticN=%zu B_tlsN=%zu "
         "field=%p loadFromHeapField=%d",
         holder, holderValid, holderMarked, verdict, aAny, aFamily, aSlot, aStack.found, aStatic.found, aConc.found,
         aFinR.found, aFinQ.found, aExport.found, aStack.visitN, aStatic.visitN, aConc.visitN, aFinR.visitN,
         aFinQ.visitN, aExport.visitN, bAny, bFamily, bSlot, bStack.found, bStatic.found, bTls.found, bStack.visitN,
         bStatic.visitN, bTls.visitN, fieldAddr, loadFromHeapField);

    // One-line miss detail when ENUM misses stack
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
