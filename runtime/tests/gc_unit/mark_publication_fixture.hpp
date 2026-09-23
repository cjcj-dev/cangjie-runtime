// Product mark-domain fixture: observes the real M3 stripe carrier.
#ifndef MRT_MARK_PUBLICATION_FIXTURE_HPP
#define MRT_MARK_PUBLICATION_FIXTURE_HPP
#include "gc_worker_fixture.hpp"
#include "gc_cycle_sequence_fixture.hpp"
#include "Heap/z/zMark.hpp"
#include "Heap/z/zDriver.hpp"
#include "gc_heap_fixture.hpp"
#include "Heap/z/zMark.hpp"
namespace MapleRuntime {
struct MarkPublicationFixture {
    inline static MarkPublicationFixture* current = nullptr;
    MarkPublicationFixture* previousFixture = current;
    static MarkPublicationFixture& Current() { CHECK(current != nullptr); return *current; }
    Heap& collector = Heap::GetHeap();
    MarkPublicationFixture()
    {
        current = this;
        if (Heap::GetHeap().GetZGeneration(ZGenerationId::young).Workers() == nullptr) {
            Heap::GetHeap().GetZGeneration(ZGenerationId::young).InitializeWorkers(1);
        }
        if (Heap::GetHeap().GetZGeneration(ZGenerationId::old).Workers() == nullptr) {
            Heap::GetHeap().GetZGeneration(ZGenerationId::old).InitializeWorkers(1);
        }
        auto& young = Heap::GetHeap().GetZGeneration(ZGenerationId::young);
        auto& old = Heap::GetHeap().GetZGeneration(ZGenerationId::old);
        // ZGenerationYoung::mark_start advances the sequence with the remset
        // flip (zGeneration.cpp:855-881), before mark work can be published.
        GenerationSequenceFixture::AdvanceYoung(young);
        Heap::GetHeap().young().Mark().BindWorkers(Heap::GetHeap().young().Workers());
        Heap::GetHeap().young().Mark().Start();
        GC_EXPECT_TRUE(Heap::GetHeap().young().Mark().Stripes().IsEmpty());
        young.set_phase(ZGenerationPhase::Mark);
        GenerationSequenceFixture::Advance(old);
        Heap::GetHeap().old().Mark().BindWorkers(Heap::GetHeap().old().Workers());
        Heap::GetHeap().old().Mark().Start();
        old.set_phase(ZGenerationPhase::Mark);
    }
    ~MarkPublicationFixture()
    {
        Drain([](BaseObject*, bool) {});
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
        DrainDomain(*Heap::GetHeap().old().MarkPtr(), std::forward<Visitor>(visitor));
    }
    void CompleteOldMarkForAdmissionTest()
    {
        Heap::GetHeap().GetZGeneration(ZGenerationId::old).set_phase(ZGenerationPhase::MarkComplete);
    }
    template<class Visitor> void Drain(Visitor&& visitor)
    {
        DrainDomain(*Heap::GetHeap().young().MarkPtr(), visitor);
        DrainDomain(*Heap::GetHeap().old().MarkPtr(), visitor);
    }
    template<class Stack> void DrainObjects(Stack& stack)
    {
        Drain([&](BaseObject* object, bool) { stack.push_back(object); });
    }
    size_t YoungPending() const {
        auto* mark = Heap::GetHeap().young().MarkPtr();
        return mark->Stripes().Population() + mark->Stacks().Population();
    }
    size_t OldPending() const {
        auto* mark = Heap::GetHeap().old().MarkPtr();
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
