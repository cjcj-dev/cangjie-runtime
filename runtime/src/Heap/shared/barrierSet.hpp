#ifndef MRT_BARRIER_SET_HPP
#define MRT_BARRIER_SET_HPP

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

    explicit BarrierSet(Name kind) : _kind(kind) {}
    virtual ~BarrierSet() = default;
    Name kind() const { return _kind; }
    static BarrierSet* barrier_set() { return _barrier_set; }
    static void set_barrier_set(BarrierSet* barrier_set);

    virtual void on_thread_create(ThreadGCData& data) = 0;
    virtual void on_thread_destroy(ThreadGCData& data) = 0;
    virtual void on_thread_attach(ThreadGCData& data, Mutator* owner, ThreadLocalData* native) = 0;
    virtual void on_thread_detach(ThreadGCData& data) = 0;
    virtual void on_slowpath_allocation_exit(BaseObject* new_obj) = 0;

private:
    static BarrierSet* _barrier_set;
    const Name _kind;
};

template<> struct BarrierSet::GetType<BarrierSet::ZBarrierSetKind> {
    using type = ZBarrierSet;
};
}
#endif
