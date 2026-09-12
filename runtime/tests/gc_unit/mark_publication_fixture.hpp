// Product mark-domain fixture: observes the real M3 stripe carrier.
#ifndef MRT_MARK_PUBLICATION_FIXTURE_HPP
#define MRT_MARK_PUBLICATION_FIXTURE_HPP
#include "Heap/Collector/CollectorProxy.h"
#include "Heap/WCollector/WCollector.h"
namespace MapleRuntime {
struct MarkPublicationFixture {
    inline static MarkPublicationFixture* current = nullptr;
    MarkPublicationFixture* previousFixture = current;
    static MarkPublicationFixture& Current() { CHECK(current != nullptr); return *current; }
    CollectorResources& resources = Heap::GetHeap().GetCollectorResources();
    WCollector collector { Heap::GetHeap().GetAllocator(), resources };
    GCWorkers young { GCWorkers::Generation::YOUNG, 1 };
    GCWorkers old { GCWorkers::Generation::OLD, 1 };
    GCWorkers* previousYoung;
    GCWorkers* previousOld;
    TracingCollector* previousCollector;
    bool previousStarted;
    MarkPublicationFixture()
        : previousYoung(resources.youngWorkers), previousOld(resources.oldWorkers),
          previousCollector(resources.collectorProxy.currentCollector), previousStarted(resources.IsGcStarted())
    {
        current = this;
        resources.youngWorkers = &young;
        resources.oldWorkers = &old;
        resources.collectorProxy.currentCollector = &collector;
        resources.SetGcStarted(true);
        collector.SelectCycle(GC_REASON_YOUNG);
        collector.youngCycle.Begin(1);
        collector.StartYoungMarkWork();
        collector.youngCycle.PublishPhase(GC_PHASE_TRACE);
        collector.oldCycle.SelectReason(GC_REASON_USER);
        collector.oldCycle.Begin(2);
        collector.StartOldMarkWork();
        collector.oldCycle.PublishPhase(GC_PHASE_TRACE);
    }
    ~MarkPublicationFixture()
    {
        Drain([](BaseObject*, bool) {});
        resources.SetGcStarted(previousStarted);
        resources.collectorProxy.currentCollector = previousCollector;
        resources.youngWorkers = previousYoung;
        resources.oldWorkers = previousOld;
        current = previousFixture;
    }
    template<class Visitor> void DrainDomain(MarkDomain& domain, Visitor&& visitor)
    {
        MarkStackEntry entry;
        for (size_t stripe = 0; stripe < domain.Stripes().Count(); ++stripe) {
            while (domain.Stacks(0).Pop(domain.Smr(), 0, domain.Stripes(), stripe, entry)) {
                visitor(entry.object(), entry.follow());
            }
        }
    }
    template<class Visitor> void DrainOld(Visitor&& visitor)
    {
        DrainDomain(*collector.majorMarkDomain, std::forward<Visitor>(visitor));
    }
    bool FollowYoung(TracingCollector::WorkStack& work, std::vector<BaseObject*>& reached)
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
        DrainDomain(*collector.youngMarkDomain, visitor);
        DrainDomain(*collector.majorMarkDomain, visitor);
    }
    template<class Stack> void DrainObjects(Stack& stack)
    {
        Drain([&](BaseObject* object, bool) { stack.push_back(object); });
    }
    size_t YoungPending() const { return collector.youngMarkDomain->Stripes().Population(); }
    size_t OldPending() const { return collector.majorMarkDomain->Stripes().Population(); }
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
