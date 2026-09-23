#ifndef MRT_HANDLE_H
#define MRT_HANDLE_H
namespace MapleRuntime {
class Mutator;
class BaseObject;
class RootSlot;
// Thread-private indirection, paired with HandleMark (HotSpot handles.hpp:65).
class Handle {
    RootSlot* slot = nullptr;
public:
    Handle() = default;
    Handle(Mutator* mutator, BaseObject* object);
    BaseObject* operator()() const;
};

} // namespace MapleRuntime
#endif
