// Product mark-domain fixture: observes the real M3 stripe carrier.
#ifndef MRT_MARK_PUBLICATION_FIXTURE_HPP
#define MRT_MARK_PUBLICATION_FIXTURE_HPP
#include "gc_worker_fixture.hpp"
#include "gc_cycle_sequence_fixture.hpp"
#include "Heap/Collector/CollectorProxy.h"
#include "gc_heap_fixture.hpp"
#include "Heap/WCollector/WCollector.h"
namespace MapleRuntime {
struct MarkPublicationFixture {
    inline static MarkPublicationFixture* current = nullptr;
    MarkPublicationFixture* previousFixture = current;
    static MarkPublicationFixture& Current() { CHECK(current != nullptr); return *current; }
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector { Heap::GetHeap().GetAllocator(), resources };
    CopyCollector* previousCollector;
    MarkPublicationFixture()
        : previousCollector(resources.collectorProxy.currentCollector)
    {
        current = this;
        collector.youngCycle.InitializeWorkers(1);
        collector.oldCycle.InitializeWorkers(1);
        if (previousCollector != nullptr) {
            GcUnit::GcHeapFixture::AdoptGenerationIdentity(collector, *previousCollector);
        }
        resources.collectorProxy.currentCollector = &collector;
        collector.youngCycle.SelectReason(GC_REASON_YOUNG);
        collector.youngCycle.Begin(1);
        // ZGenerationYoung::mark_start advances the sequence with the remset
        // flip (zGeneration.cpp:855-881), before mark work can be published.
        GenerationSequenceFixture::AdvanceYoung(collector.youngCycle);
        collector.StartYoungMarkWork();
        collector.youngCycle.PublishPhase(GC_PHASE_ENUM);
        collector.oldCycle.SelectReason(GC_REASON_USER);
        collector.oldCycle.Begin(2);
        GenerationSequenceFixture::Advance(collector.oldCycle);
        collector.StartOldMarkWork();
        collector.oldCycle.PublishPhase(GC_PHASE_ENUM);
    }
    ~MarkPublicationFixture()
    {
        Drain([](BaseObject*, bool) {});
        resources.collectorProxy.currentCollector = previousCollector;
        current = previousFixture;
    }
    template<class Visitor> void DrainDomain(ZMark& domain, Visitor&& visitor)
    {
        GcUnit::WorkerFixture worker;
        // ZMark::flush publishes the mutator's partial stack before workers drain it.
        ThreadLocal::FlushMarkStacks(ThreadLocal::GetThreadLocalData(), domain);
        MarkStackEntry entry;
        for (size_t stripe = 0; stripe < domain.Stripes().Count(); ++stripe) {
            while (domain.Stacks().Pop(domain.Smr(), 0, domain.Stripes(), stripe, entry)) {
                visitor(to_object(ZOffset::address(to_zoffset(entry.object_address()))), entry.follow());
            }
        }
    }
    template<class Visitor> void DrainOld(Visitor&& visitor)
    {
        DrainDomain(*collector.MajorMark(), std::forward<Visitor>(visitor));
    }
    bool FollowYoung(WorkStack& work, std::vector<BaseObject*>& reached)
    {
        WCollector::MinorSlotSet slots;
        WCollector::MinorSlotSet weakSlots;
        return collector.FollowYoungMark(work, false, reached, slots, weakSlots);
    }
    void CompleteOldMarkForAdmissionTest()
    {
        collector.oldCycle.PublishPhase(GC_PHASE_MARK_COMPLETE);
    }
    template<class Visitor> void Drain(Visitor&& visitor)
    {
        DrainDomain(*collector.YoungMark(), visitor);
        DrainDomain(*collector.MajorMark(), visitor);
    }
    template<class Stack> void DrainObjects(Stack& stack)
    {
        Drain([&](BaseObject* object, bool) { stack.push_back(object); });
    }
    size_t YoungPending() const {
        auto* mark = const_cast<WCollector&>(collector).YoungMark();
        return mark->Stripes().Population() + mark->Stacks().Population();
    }
    size_t OldPending() const {
        auto* mark = const_cast<WCollector&>(collector).MajorMark();
        return mark->Stripes().Population() + mark->Stacks().Population();
    }
};
template<class Stack> void DrainPublishedMarkObjects(Stack& stack)
{
    MarkPublicationFixture::Current().DrainObjects(stack);
}
template<class Visitor> void DrainPublishedMarkEntries(Visitor&& visitor)
{
    MarkPublicationFixture::Current().Drain(std::forward<Visitor>(visitor));
}
}
#endif
