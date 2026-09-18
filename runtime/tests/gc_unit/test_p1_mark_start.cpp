// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
#include <array>
#include <cstdio>
#include <dlfcn.h>
#include "Heap/Allocator/RegionSpace.h"
#include "Heap/z/zHeap.hpp"
#include "Heap/z/zMark.hpp"
#include "ObjectModel/MObject.h"
#include "TypeInfoManager.h"

namespace MapleRuntime {
// Read-only access to the existing product remset face. This test never seeds
// intermediate phase, mark, queue, or current-address state.
struct ZGenerationRootTestAccess {
    static unsigned RemsetFace()
    {
        return 0;
    }
};
}
using namespace MapleRuntime;
namespace {
struct StartState {
    uint64_t sequence = 0;
    uintptr_t color = 0;
    uintptr_t finalizable = 0;
    unsigned face = 0;
    size_t starts = 0;
    size_t completes = 0;
};
unsigned failures = 0;
void Expect(bool value, const char* invariant)
{
    std::printf("P1_ASSERT %s %s\n", invariant, value ? "PASS" : "FAIL");
    std::fflush(stdout);
    failures += !value;
}
}

// Entered from a compiler-generated managed frame. All startup and collection
// work is performed by the normal runtime, via the real synchronous request.
extern "C" int p1MarkStartExercise()
{
    failures = 0;
    Collector& collector = Heap::GetHeap().GetCollector();
    auto& resources = Heap::GetHeap().GetCollectorResources();
    alignas(TypeInfo) static unsigned char storage[sizeof(TypeInfo)] {};
    auto* type = reinterpret_cast<TypeInfo*>(storage);
    type->SetType(TypeKind::TYPE_KIND_CLASS);
    type->SetInstanceSize(sizeof(uint64_t));
    TypeInfoManager::GetTypeInfoManager().NoteTypeInfoImage(reinterpret_cast<uintptr_t>(storage), sizeof(storage));
    auto* object = MObject::NewObject(type, 16, AllocType::MOVEABLE_OBJECT);
    const U64 handle = Heap::GetHeap().RegisterExportRoot(object);
    ZPage* page = Heap::page(reinterpret_cast<MAddress>(object));
    Expect(page->IsAllocating(), "real_allocation_has_current_birth");
    std::printf("P1_ALLOCATED page=%p birth=%llu owner_sequence=%llu\n", page,
                static_cast<unsigned long long>(page->BirthSequence()),
                static_cast<unsigned long long>(page->GetSnapshotEpoch()));

    std::array<StartState, 2> state {};
    bool youngComplete = false;
    Collector::testMarkStartState = [&](ZGenerationId generation, MarkStartPoint point,
                                               const ZMark* domain) {
        const size_t index = generation == ZGenerationId::young ? 0 : 1;
        auto& before = state[index];
        const auto snapshot = collector.GetCycleSnapshot(generation);
        const uintptr_t mask = index == 0 ? ZPointerMarkedYoungMask : ZPointerMarkedOldMask;
        const uintptr_t color = ::g_cjMarkBadMask & mask;
        const unsigned face = ZGenerationRootTestAccess::RemsetFace();
        const unsigned workers = resources.GetWorkers(generation).ActiveWorkers();
        std::printf("P1_PRODUCT_STATE gen=%zu point=%u seq=%llu phase=%u color=%zx face=%u domain=%p domain_workers=%zu workers=%u\n",
                    index, static_cast<unsigned>(point), static_cast<unsigned long long>(snapshot.sequence),
                    static_cast<unsigned>(snapshot.phase), color, face, domain,
                    domain == nullptr ? size_t(0) : domain->NWorkers(), workers);
        if (point == MarkStartPoint::Begin) {
            before.sequence = snapshot.sequence;
            before.color = color;
            before.finalizable = ZPointerFinalizable;
            before.face = face;
            ++before.starts;
            if (index == 0) youngComplete = false;
            else Expect(youngComplete, "young_completes_before_old_starts");
        } else if (point == MarkStartPoint::BeforeRetire) {
            Expect(color != before.color, index == 0 ? "young_color_before_retire" : "old_color_before_retire");
            Expect(ZPointerFinalizable == (before.finalizable ^ (index == 0 ? 0 : ZPointerFinalizableMask)),
                   index == 0 ? "young_preserves_finalizable_epoch" : "old_finalizable_before_retire");
            Expect(snapshot.sequence == before.sequence, "retirement_precedes_sequence");
        } else if (point == MarkStartPoint::BeforeSequence) {
            Expect(snapshot.sequence == before.sequence, "sequence_unchanged_while_retiring");
            if (index == 0) {
                bool retired = true;
                size_t buffers = 0;
                Heap::GetHeap().GetAllocator().VisitAllocBuffers([&](AllocBuffer& buffer) {
                    ++buffers;
                    const auto empty = [](ZPage* region) {
                        return region == nullptr || region == ZPage::NullRegion();
                    };
                    retired = retired && empty(buffer.GetRegion()) && empty(buffer.GetPreparedRegion());
                });
                std::printf("P1_PRODUCT_TLAB buffers=%zu retired=%u\n", buffers, retired);
                Expect(buffers != 0, "retirement_has_real_mutator_inputs");
                Expect(retired, "current_and_prepared_tlabs_retired");
            }
        } else if (point == MarkStartPoint::BeforeDomain) {
            Expect(snapshot.sequence == before.sequence + 1 && snapshot.phase == ZGenerationPhase::Mark,
                   "sequence_and_mark_phase_before_domain");
            if (index == 0) Expect(face == before.face, "young_remset_unchanged_before_domain");
        } else if (point == MarkStartPoint::BeforeRemembered) {
            Expect(domain != nullptr && domain->NWorkers() == workers, "young_domain_ready_before_remset");
            Expect(face == before.face, "young_remset_unchanged_after_domain_start");
        } else if (point == MarkStartPoint::Complete) {
            Expect(snapshot.sequence == before.sequence + 1 && snapshot.phase == ZGenerationPhase::Mark,
                   "completed_start_has_new_identity_and_mark_phase");
            Expect(domain != nullptr && domain->NWorkers() == workers, "completed_start_has_prepared_domain");
            Expect(face == (before.face ^ (index == 0 ? 1U : 0U)), "only_young_start_flips_remset");
            ++before.completes;
            if (index == 0) youngComplete = true;
            std::printf("P1_MARK_START_PHASE_RESULT gen=%zu failures=%u starts=%zu completes=%zu\n",
                        index, failures, before.starts, before.completes);
            std::fflush(stdout);
        }
    };
    const auto youngBefore = collector.GetCycleSnapshot(ZGenerationId::young);
    const auto oldBefore = collector.GetCycleSnapshot(ZGenerationId::old);
    collector.RequestGC(GC_REASON_USER, false);
    const auto oldAfterMajor = collector.GetCycleSnapshot(ZGenerationId::old);
    collector.RequestGC(GC_REASON_YOUNG, false);
    const auto youngAfter = collector.GetCycleSnapshot(ZGenerationId::young);
    const auto oldAfter = collector.GetCycleSnapshot(ZGenerationId::old);
    Collector::testMarkStartState = nullptr;
    Expect(oldAfterMajor.sequence > oldBefore.sequence, "major_request_started_old");
    Expect(oldAfter.sequence == oldAfterMajor.sequence, "minor_preserves_old_identity");
    Expect(state[0].starts == youngAfter.sequence - youngBefore.sequence && state[0].starts != 0,
           "young_sequence_delta_matches_real_starts");
    Expect(state[1].starts == oldAfter.sequence - oldBefore.sequence && state[1].starts != 0,
           "old_sequence_delta_matches_real_starts");
    Expect(state[0].starts == state[0].completes && state[1].starts == state[1].completes,
           "every_started_generation_completed");
    auto* current = Heap::GetHeap().GetExportObject(handle);
    Expect(current != nullptr && Heap::IsHeapAddress(current), "export_root_survives_both_requests");
    Heap::GetHeap().RemoveExportObject(handle);
    std::printf("P1_MARK_START_RESULT failures=%u young_starts=%zu old_starts=%zu\n",
                failures, state[0].starts, state[1].starts);
    return static_cast<int>(failures);
}
