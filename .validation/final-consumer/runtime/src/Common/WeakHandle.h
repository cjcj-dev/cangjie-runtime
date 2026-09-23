#ifndef MRT_WEAK_HANDLE_H
#define MRT_WEAK_HANDLE_H

#include "Common/OopStorage.h"
#include "ObjectModel/RefField.h"

namespace MapleRuntime {
class WeakHandle {
private:
    NativeSlot* obj;
    explicit WeakHandle(NativeSlot* slot) : obj(slot) {}
public:
    WeakHandle() : obj(nullptr) {}
    WeakHandle(OopStorage* storage, BaseObject* object);
    BaseObject* resolve() const;
    BaseObject* peek() const;
    void replace(BaseObject* withObj);
    void release(OopStorage* storage);
    bool is_null() const { return obj == nullptr; }
    bool is_empty() const { return obj == nullptr; }
    NativeSlot* ptr_raw() const { return obj; }
};
} // namespace MapleRuntime
#endif
