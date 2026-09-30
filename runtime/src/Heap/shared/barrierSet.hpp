#ifndef MRT_BARRIER_SET_HPP
#define MRT_BARRIER_SET_HPP

#include "Base/fakeRttiSupport.hpp"

namespace MapleRuntime {
class BaseObject;
class Mutator;
struct ThreadLocalData;
struct ThreadGCData;
class ZBarrierSet;

class BarrierSet {
public:
    enum Name { ZBarrierSetKind };
    template<Name kind> struct GetType;
    template<typename BarrierSetT> struct GetName;

protected:
    using FakeRtti = FakeRttiSupport<BarrierSet, Name>;

public:

    explicit BarrierSet(const FakeRtti& fake_rtti) : _fake_rtti(fake_rtti) {}
    virtual ~BarrierSet() = default;
    Name kind() const { return _fake_rtti.concrete_tag(); }
    static BarrierSet* barrier_set() { return _barrier_set; }
    static void set_barrier_set(BarrierSet* barrier_set);

    virtual void on_thread_create(ThreadGCData& data) = 0;
    virtual void on_thread_destroy(ThreadGCData& data) = 0;
    virtual void on_thread_attach(ThreadGCData& data, Mutator* owner, ThreadLocalData* native) = 0;
    virtual void on_thread_detach(ThreadGCData& data) = 0;
    virtual void on_slowpath_allocation_exit(BaseObject* new_obj) = 0;

private:
    static BarrierSet* _barrier_set;
    FakeRtti _fake_rtti;
};

template<> struct BarrierSet::GetType<BarrierSet::ZBarrierSetKind> {
    using type = ZBarrierSet;
};

template<> struct BarrierSet::GetName<ZBarrierSet> {
    static constexpr Name value = ZBarrierSetKind;
};
}
#endif
